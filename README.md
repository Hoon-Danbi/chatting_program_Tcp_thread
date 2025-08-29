패킷 기반 멀티클라이언트 채팅 프로그램
==================================

0\. 개요
------

*   **목표**: TCP 스트림 위에 **직접 설계한 패킷 프로토콜**을 구현하여, 다수 클라이언트가 참여하는 **채팅 서버/클라이언트**를 완성한다.

* * *

1\. 프로토콜 명세
-------------------

### 1.1 공통 헤더 (고정 길이)

*   모든 멀티바이트 필드는 **네트워크 바이트 오더(빅엔디언)**.
*   텍스트는 **UTF-8**, `sender`는 고정 32바이트, 널 종료를 **필수로 요구하지 않음**.

```c
#pragma pack(push, 1)
typedef struct {
    uint16_t ver;        // 프로토콜 버전 (예: 1)
    uint16_t type;       // 메시지 유형 (아래 enum)
    uint32_t length;     // payload 길이(바이트), 헤더 제외
    uint32_t seq;        // 송신 시퀀스 번호 (클라/서버가 각자 증가)
    uint64_t ts_ms;      // Unix epoch milliseconds
    uint32_t room_id;    // 채팅방 ID (기본 1)
    char     sender[32]; // 닉네임(최대 31바이트 + 미널 종료 가능/불가)
} pkt_hdr_t;
#pragma pack(pop)
```

### 1.2 메시지 타입

```c
enum pkt_type {
    PKT_JOIN = 1,   // payload: 없음(또는 선택적 데이터)
    PKT_MSG  = 2,   // payload: 채팅 텍스트(바이너리 허용)
    PKT_LEAVE= 3,   // payload: 없음
    PKT_NICK = 4,   // payload: 새 닉네임 텍스트
    PKT_SYS  = 5,   // payload: 서버 공지(브로드캐스트용)
    PKT_PING = 6,   // payload: 없음
    PKT_PONG = 7    // payload: 없음
};
```

### 1.3 전송 규칙

*   송신: **헤더(고정)** → **payload(length 바이트)**.
*   수신: `recv_exact()` 로 **헤더를 정확히 읽고** → `ntoh*` 변환 → `length`만큼 **payload를 정확히 읽는다**.
*   텍스트 출력 시 **널 종료를 가정하지 말고** 수신 길이 기반으로 출력.

* * *

2\. 기능 요구사항
-----------

### 2.1 서버

*   TCP 리스닝(포트 자유).
*   최소 10명 이상의 동시 접속 처리.
    *   방법: `fork / thread / select` 중 택1
*   **JOIN/NICK/MSG/LEAVE/PING/PONG** 구현.
*   **방 단위 브로드캐스트**: 동일 `room_id`에만 전달.

### 2.2 클라이언트

*   서버와 TCP 연결.
*   표준입력과 서버 소켓을 **동시에 대기**(`select` 권장).
*   명령:
    *   `/nick NAME` → 닉네임 변경(PKT\_NICK)
    *   `/join ROOM` → 방 변경(PKT\_JOIN)
    *   `/ping` → PING/PONG 왕복 확인
    *   `/quit` → 종료(PKT\_LEAVE 송신 후 종료)
*   일반 텍스트 입력은 **PKT\_MSG** 로 송신.

### 2.3 로깅

*   서버: 접속/퇴장, 닉변, 방 이동, 에러 로그.
*   클라: 수신 메시지 `[room][sender] message` 형태 출력.

* * *



3. 빌드 & 실행 예시
----------

```bash
# 빌드
gcc -O2 -Wall -pthread -o chat_server_thread chat_server_thread.c
gcc -O2 -Wall -pthread -o chat_client_thread chat_client_thread.c

# 실행
./chat_server_thread
./chat_client_thread 127.0.0.1 alice
./chat_client_thread 127.0.0.1 bob
```

*   클라이언트 명령: `/nick NAME`, `/join ROOM`, `/ping`, `/quit`
*   출력 형식: `[room][sender] message` (서버 공지는 PKT\_SYS로 단순 출력)
