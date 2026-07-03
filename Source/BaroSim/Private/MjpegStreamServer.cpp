// Fill out your copyright notice in the Description page of Project Settings.

#include "MjpegStreamServer.h"

#include "Common/TcpListener.h"   // FTcpListener (pulls SocketSubsystem/IPv4Endpoint/RunnableThread)
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogMjpegStream, Log, All);

namespace
{
	const TCHAR* const kBoundary = TEXT("baroframe");
}

FMjpegStreamServer::FMjpegStreamServer()
{
}

FMjpegStreamServer::~FMjpegStreamServer()
{
	StopServer();
}

bool FMjpegStreamServer::StartServer(int32 Port, int32 InFps)
{
	Fps = FMath::Clamp(InFps, 1, 60);
	bStop = false;

	// auto-reset: 송신 중 도착한 프레임도 다음 Wait 가 즉시 리턴해 놓치지 않는다.
	NewFrameEvent = FPlatformProcess::GetSynchEventFromPool(false);

	const FIPv4Endpoint Endpoint(FIPv4Address::Any, static_cast<uint16>(Port));
	// 짧은 폴링 간격으로 accept 응답성 확보.
	Listener = new FTcpListener(Endpoint, FTimespan::FromMilliseconds(200));
	Listener->OnConnectionAccepted().BindRaw(this, &FMjpegStreamServer::HandleConnection);

	Thread = FRunnableThread::Create(this, TEXT("HucomsMjpegStream"), 0, TPri_Normal);
	if (!Thread)
	{
		UE_LOG(LogMjpegStream, Error, TEXT("[MJPEG] 워커 스레드 생성 실패"));
		StopServer();
		return false;
	}

	UE_LOG(LogMjpegStream, Log, TEXT("[MJPEG] 연속 스트림 서버 시작 :%d (fps=%d)"), Port, Fps);
	return true;
}

void FMjpegStreamServer::StopServer()
{
	bStop = true;
	if (NewFrameEvent)
	{
		NewFrameEvent->Trigger(); // 대기 중인 워커를 즉시 깨워 종료
	}

	// 리스너 먼저 정지(신규 accept 차단). 소멸자가 accept 스레드 kill + listen 소켓 정리.
	if (Listener)
	{
		delete Listener;
		Listener = nullptr;
	}

	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	if (NewFrameEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(NewFrameEvent);
		NewFrameEvent = nullptr;
	}

	CloseAllClients();
}

bool FMjpegStreamServer::HandleConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint)
{
	if (!Socket)
	{
		return false;
	}
	Socket->SetNonBlocking(false);
	{
		FScopeLock P(&PendingLock);
		Pending.Add(Socket);
	}
	UE_LOG(LogMjpegStream, Log, TEXT("[MJPEG] 클라이언트 연결: %s"), *Endpoint.ToString());
	return true; // 소유권 인수
}

void FMjpegStreamServer::UpdateFrame(const TArray<uint8>& Jpeg)
{
	{
		FScopeLock F(&FrameLock);
		LatestFrame = Jpeg;
		++FrameSeq;
	}
	if (NewFrameEvent)
	{
		NewFrameEvent->Trigger();
	}
}

int32 FMjpegStreamServer::GetClientCount() const
{
	int32 Count = 0;
	{
		FScopeLock C(&ClientsLock);
		Count += Clients.Num();
	}
	{
		FScopeLock P(&PendingLock);
		Count += Pending.Num();
	}
	return Count;
}

bool FMjpegStreamServer::HasClients() const
{
	{
		FScopeLock C(&ClientsLock);
		if (Clients.Num() > 0) { return true; }
	}
	{
		FScopeLock P(&PendingLock);
		if (Pending.Num() > 0) { return true; }
	}
	return false;
}

bool FMjpegStreamServer::SendAll(FSocket* S, const uint8* Data, int32 Len)
{
	int32 Total = 0;
	while (Total < Len)
	{
		int32 Sent = 0;
		if (!S->Send(Data + Total, Len - Total, Sent) || Sent <= 0)
		{
			return false;
		}
		Total += Sent;
	}
	return true;
}

void FMjpegStreamServer::DestroyClientSocket(FSocket* S)
{
	if (!S)
	{
		return;
	}
	S->Close();
	if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		SS->DestroySocket(S);
	}
}

void FMjpegStreamServer::CloseAllClients()
{
	{
		FScopeLock C(&ClientsLock);
		for (FClient& Cl : Clients) { DestroyClientSocket(Cl.Socket); }
		Clients.Reset();
	}
	{
		FScopeLock P(&PendingLock);
		for (FSocket* S : Pending) { DestroyClientSocket(S); }
		Pending.Reset();
	}
}

uint32 FMjpegStreamServer::Run()
{
	const FString HeaderStr = FString::Printf(
		TEXT("HTTP/1.1 200 OK\r\n")
		TEXT("Content-Type: multipart/x-mixed-replace; boundary=%s\r\n")
		TEXT("Cache-Control: no-cache\r\n")
		TEXT("Pragma: no-cache\r\n")
		TEXT("Connection: close\r\n\r\n"),
		kBoundary);

	while (!bStop)
	{
		// 1) 대기열 -> 활성 (중첩 락 없이)
		TArray<FSocket*> NewOnes;
		{
			FScopeLock P(&PendingLock);
			NewOnes = MoveTemp(Pending);
			Pending.Reset();
		}
		if (NewOnes.Num() > 0)
		{
			FScopeLock C(&ClientsLock);
			for (FSocket* S : NewOnes) { Clients.Add(FClient{ S, false }); }
		}

		// 2) 최신 프레임 + 시퀀스 스냅샷
		TArray<uint8> Frame;
		uint64 Seq = 0;
		{
			FScopeLock F(&FrameLock);
			Frame = LatestFrame;
			Seq = FrameSeq;
		}

		// 3) 이 시퀀스를 아직 못 받은 클라이언트에만 송신 (중복 재전송 없음 —
		//    페이싱은 producer 의 UpdateFrame 주기가 결정)
		//    송신 중에는 ClientsLock 을 잡지 않는다: 블로킹 SendAll 이 락을 문 채 멈추면
		//    게임스레드의 HasClients() 가 같이 멈춰 sim 전체가 얼어붙는다(느린 원격 브라우저).
		//    Clients 의 구조 변경(추가/제거)은 이 워커 스레드만 수행하므로 락은 원소
		//    접근/변경 순간만 잡으면 HasClients()/CloseAllClients() 와 안전하다.
		if (Frame.Num() > 0)
		{
			const FString PartHdr = FString::Printf(
				TEXT("--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"),
				kBoundary, Frame.Num());

			int32 Count = 0;
			{
				FScopeLock C(&ClientsLock);
				Count = Clients.Num();
			}
			for (int32 i = Count - 1; i >= 0; --i)
			{
				FClient Snapshot;
				{
					FScopeLock C(&ClientsLock);
					Snapshot = Clients[i];
				}
				if (Snapshot.SentSeq == Seq)
				{
					continue;
				}
				bool bOk = true;

				if (!Snapshot.bHeaderSent)
				{
					FTCHARToUTF8 H(*HeaderStr);
					bOk = SendAll(Snapshot.Socket, reinterpret_cast<const uint8*>(H.Get()), H.Length());
				}
				if (bOk)
				{
					FTCHARToUTF8 PH(*PartHdr);
					bOk = SendAll(Snapshot.Socket, reinterpret_cast<const uint8*>(PH.Get()), PH.Length());
				}
				if (bOk)
				{
					bOk = SendAll(Snapshot.Socket, Frame.GetData(), Frame.Num());
				}
				if (bOk)
				{
					const uint8 Trailer[2] = { '\r', '\n' };
					bOk = SendAll(Snapshot.Socket, Trailer, 2);
				}

				FScopeLock C(&ClientsLock);
				if (bOk)
				{
					Clients[i].bHeaderSent = true;
					Clients[i].SentSeq = Seq;
				}
				else
				{
					UE_LOG(LogMjpegStream, Log, TEXT("[MJPEG] 클라이언트 연결 종료(송신 실패) -> 정리"));
					DestroyClientSocket(Clients[i].Socket);
					Clients.RemoveAt(i);
				}
			}
		}

		// 4) 새 프레임(또는 정지)까지 대기 — 고정 sleep 과 달리 송신 시간이 주기를 깎지 않는다.
		//    타임아웃은 신규 accept(Pending) 흡수용 폴링 간격.
		if (NewFrameEvent)
		{
			NewFrameEvent->Wait(FTimespan::FromMilliseconds(200));
		}
		else
		{
			FPlatformProcess::Sleep(0.2f);
		}
	}

	return 0;
}

void FMjpegStreamServer::Stop()
{
	bStop = true;
	if (NewFrameEvent)
	{
		NewFrameEvent->Trigger();
	}
}
