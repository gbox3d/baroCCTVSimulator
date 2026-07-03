// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/CriticalSection.h"
#include <atomic>

class FSocket;
class FTcpListener;
class FRunnableThread;
class FEvent;
struct FIPv4Endpoint;

/**
 * FMjpegStreamServer — 연속 MJPEG(multipart/x-mixed-replace) TCP 스트리밍 서버.
 *
 * UE 의 IHttpRouter 는 단일 응답만 가능해 연속 스트림을 못 하므로, RTSP 브리지(ffmpeg/MediaMTX)가
 * 읽어갈 연속 MJPEG 를 전용 소켓 스레드로 직접 서빙한다.
 *  - FTcpListener 가 accept (자체 스레드).
 *  - 워커 스레드가 연결된 클라이언트에 "새" JPEG 프레임만 multipart 청크로 송신.
 *    페이싱은 producer(게임스레드) 가 결정 — UpdateFrame() 이 이벤트로 워커를 깨우고,
 *    워커는 프레임 시퀀스가 갱신된 경우에만 송신한다(중복 재전송·고정 sleep 없음).
 *  - 게임스레드(UHucomsServerSubsystem::Tick)가 UpdateFrame() 으로 최신 프레임을 갱신.
 *
 * 소비자: MediaMTX 의 runOnDemand ffmpeg 가 http://127.0.0.1:<port>/ 를 mpjpeg 로 읽어 RTSP/554 로 재송출.
 * 주의(v1): 블로킹 송신 + localhost 소비자 가정. 느린 원격 클라이언트는 대상 아님.
 */
class FMjpegStreamServer : public FRunnable
{
public:
	FMjpegStreamServer();
	virtual ~FMjpegStreamServer();

	/** Port 에서 accept 시작 + 워커 스레드 기동. */
	bool StartServer(int32 Port, int32 InFps);

	/** 리스너/워커 정지 및 모든 클라이언트 정리. */
	void StopServer();

	/** 게임스레드가 최신 JPEG 프레임을 설정(복사). */
	void UpdateFrame(const TArray<uint8>& Jpeg);

	/** 연결(또는 대기) 클라이언트가 있는지 — 게임스레드가 캡처 여부 판단에 사용. */
	bool HasClients() const;

	/** 연결(활성+대기) 클라이언트 수 — HUD 상태 표시용. */
	int32 GetClientCount() const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	/** FTcpListener accept 콜백(리스너 스레드). true 반환 시 소켓 소유권을 가져온다. */
	bool HandleConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint);

	/** 블로킹으로 전부 송신. 실패 시 false. */
	static bool SendAll(FSocket* S, const uint8* Data, int32 Len);

	void DestroyClientSocket(FSocket* S);
	void CloseAllClients();

	FTcpListener* Listener = nullptr;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStop{ false };
	int32 Fps = 15;

	struct FClient
	{
		FSocket* Socket = nullptr;
		bool bHeaderSent = false;
		/** 마지막으로 송신한 프레임 시퀀스(0 = 아직 없음 → 접속 즉시 최신 프레임 수신). */
		uint64 SentSeq = 0;
	};
	TArray<FClient> Clients;
	mutable FCriticalSection ClientsLock;

	/** accept 직후 대기열(리스너 스레드 -> 워커 스레드). */
	TArray<FSocket*> Pending;
	mutable FCriticalSection PendingLock;

	TArray<uint8> LatestFrame;
	/** UpdateFrame() 마다 증가 — 워커가 "새 프레임인가"를 판별하는 기준. FrameLock 으로 보호. */
	uint64 FrameSeq = 0;
	FCriticalSection FrameLock;

	/** 새 프레임 도착(또는 정지) 시 워커를 깨우는 auto-reset 이벤트. */
	FEvent* NewFrameEvent = nullptr;
};
