// ws_beast_set_all_annotated.cpp                                   // 파일 설명: Beast 기반 WebSocket 클라이언트 (채널 리스트/범위/all + on/off + 50ms 간격 전송 지원)
// Build: g++ -std=c++17 -O2 ws_beast_set_all_annotated.cpp -o kulgad-cli-beast -lboost_system -lpthread
//        // 참고: 보통 -lboost_thread는 필요 없음
// Run  : ./kulgad-cli-beast [host] [port] set <channels|all> <on|off> // 실행 형식(호스트/포트 생략 시 localhost:3001)

// ======= 헤더 포함부 =======
#include <boost/beast/core.hpp>                                       // Beast 핵심 타입(flat_buffer 등)
#include <boost/beast/websocket.hpp>                                  // Beast WebSocket 스트림 API
#include <boost/asio/connect.hpp>                                     // ASIO connect 유틸(여러 endpoint 중 연결)
#include <boost/asio/ip/tcp.hpp>                                      // ASIO TCP 소켓/리졸버
#include <algorithm>                                                  // std::sort, std::unique, std::remove_if
#include <cctype>                                                     // std::tolower, std::isspace
#include <chrono>                                                     // 전송 간격(50ms)용 chrono
#include <iostream>                                                   // 표준 입출력
#include <string>                                                     // std::string
#include <thread>                                                     // std::this_thread::sleep_for
#include <vector>                                                     // 채널 목록 보관용 std::vector

// ======= 네임스페이스 단축 =======
namespace beast = boost::beast;                                       // beast:: 로 축약
namespace websocket = beast::websocket;                               // websocket:: 로 축약
namespace net = boost::asio;                                          // net:: 로 축약
using tcp = boost::asio::ip::tcp;                                     // tcp::resolver, tcp::socket 등 사용 편의

// ======= 사용법 출력 함수 =======
static void usage() {
    std::cerr
      << "Usage: kulgad-cli-beast [host] [port] set <channels|all> <on|off>\n" // 사용 예시 출력
      << "  channels: 0..255, 콤마/범위 혼용 가능 (예: 0-4,7,10-12)\n"         // 채널 표기 규칙 안내
      << "  'all'    : 0..255 전부\n";                                         // all 키워드 설명
}

// ======= 인자 파싱 함수: 호스트/포트/채널목록/on|off 추출 =======
static bool parse_args(int argc, char** argv,
                       std::string& host, std::string& port,
                       std::vector<int>& channels, bool& val)
{
    host = "localhost";                                               // 기본 호스트: localhost
    port = "3001";                                                    // 기본 포트: 3001
    if (argc < 4) { usage(); return false; }                          // 인자 부족 시 사용법 출력 후 실패

    int idx = 1;                                                      // 'set' 토큰의 예상 위치 인덱스
    // 패턴 A) set <channels> <on|off>
    // 패턴 B) <host> <port> set <channels> <on|off>
    if (std::string(argv[1]) != "set") {                              // 첫 토큰이 set이 아니면
        if (argc < 6) { usage(); return false; }                      // 호스트/포트까지 주는 패턴이 아니면 실패
        host = argv[1];                                               // 호스트 추출
        port = argv[2];                                               // 포트 추출
        idx  = 3;                                                     // 이제 idx=3 위치에 set가 와야 함
        if (std::string(argv[idx]) != "set") { usage(); return false; } // set 확인 실패 시 에러
    }

    // 마지막 인자는 on/off 상태
    std::string state = argv[argc - 1];                               // 마지막 토큰
    std::transform(state.begin(), state.end(), state.begin(),
                   [](unsigned char c){ return std::tolower(c); });   // 소문자로 통일
    if (state == "on" || state == "true" || state == "1") val = true; // on/true/1 → true
    else if (state == "off" || state == "false" || state == "0") val = false; // off/false/0 → false
    else { std::cerr << "State must be on/off\n"; return false; }     // 그 외 값은 오류

    // 채널 문자열(여러 토큰 가능) 합치기
    std::string chanJoined;                                           // 채널 입력 전체를 이어붙인 문자열
    for (int i = idx + 1; i < argc - 1; ++i) {                        // set 다음 ~ 마지막 전 토큰까지
        if (!chanJoined.empty()) chanJoined.push_back(' ');           // 토큰 사이 공백 하나 삽입(나중에 제거)
        chanJoined += argv[i];                                        // 누적
    }
    // 모든 공백 제거(파싱 단순화)
    chanJoined.erase(std::remove_if(chanJoined.begin(), chanJoined.end(),
                                    [](unsigned char c){ return std::isspace(c); }),
                     chanJoined.end());
    if (chanJoined.empty()) { std::cerr << "No channels given\n"; return false; } // 비어 있으면 오류

    // 'all' 처리: 단독이든, 목록에 포함되었든 발견 시 전체 선택
    std::string lower = chanJoined;                                   // 소문자 복사본
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });   // 소문자 변환

    auto push_all = [&](){                                            // 0..255 전체 채널 추가 람다
        channels.reserve(256);                                        // capacity 최적화
        for (int ch = 0; ch <= 255; ++ch) channels.push_back(ch);     // 0~255 모두 push
    };

    if (lower == "all" || lower.find("all") != std::string::npos) {   // 'all'이 포함되면
        push_all();                                                   // 전체 채널 선택
    } else {
        // 콤마로 분리 → 각 토큰을 단일 숫자 또는 a-b 범위로 해석
        auto add_channel = [&](int ch)->bool{                         // 단일 채널 추가 + 범위 체크
            if (ch < 0 || ch > 255) {                                 // 0..255 이외는 오류
                std::cerr << "Channel out of range: " << ch << "\n";
                return false;
            }
            channels.push_back(ch);                                   // 유효하면 추가
            return true;
        };

        size_t start = 0;                                             // 파싱 시작 인덱스
        while (start < chanJoined.size()) {                           // 문자열 끝까지
            size_t pos = chanJoined.find(',', start);                 // 콤마 위치 찾기
            std::string tok = chanJoined.substr(                      // 현재 토큰 추출
                start, (pos == std::string::npos) ? std::string::npos : pos - start);

            if (!tok.empty()) {                                       // 비어있지 않다면
                size_t dash = tok.find('-');                          // 범위 구분자 '-' 탐색
                if (dash == std::string::npos) {                      // '-' 없으면 단일 숫자
                    try {
                        int ch = std::stoi(tok);                      // 정수로 변환
                        if (!add_channel(ch)) return false;           // 범위 검증/추가
                    } catch (...) {                                   // 변환 실패 시
                        std::cerr << "Invalid channel: " << tok << "\n";
                        return false;
                    }
                } else {                                              // '-' 있으면 a-b 범위
                    std::string a_str = tok.substr(0, dash);          // a 부분
                    std::string b_str = tok.substr(dash + 1);         // b 부분
                    if (a_str.empty() || b_str.empty()) {             // "a-" 또는 "-b" 같은 경우
                        std::cerr << "Invalid range: " << tok << "\n";
                        return false;
                    }
                    int a, b;
                    try { a = std::stoi(a_str); b = std::stoi(b_str); } // 정수 변환
                    catch (...) { std::cerr << "Invalid range: " << tok << "\n"; return false; }
                    if (a > b) std::swap(a, b);                       // a>b면 교환
                    if (a < 0 || b > 255) {                           // 범위 경계 확인
                        std::cerr << "Range out of bounds: " << tok << "\n";
                        return false;
                    }
                    for (int ch = a; ch <= b; ++ch) channels.push_back(ch); // a..b 모두 추가
                }
            }
            if (pos == std::string::npos) break;                      // 더 이상 콤마 없으면 종료
            start = pos + 1;                                          // 다음 토큰으로 이동
        }

        if (channels.empty()) {                                       // 유효 채널 하나도 없으면
            std::cerr << "No valid channels parsed\n";                // 오류 출력
            return false;                                             // 실패
        }
    }

    // 정렬 + 중복 제거 (e.g., all + 일부 중복, 겹치는 범위 등)
    std::sort(channels.begin(), channels.end());                      // 정렬
    channels.erase(std::unique(channels.begin(), channels.end()),     // 인접 중복 제거
                   channels.end());
    return true;                                                      // 파싱 성공
}

// ======= 메인 함수 =======
int main(int argc, char** argv) {
    try {                                                             // 예외 처리 시작
        std::string host, port;                                       // 호스트/포트 결과 변수
        std::vector<int> channels;                                    // 채널 목록
        bool val = false;                                             // on/off 값 (true=on)

        if (!parse_args(argc, argv, host, port, channels, val))       // 인자 파싱
            return 1;                                                 // 실패 시 종료

        net::io_context ioc;                                          // ASIO I/O 컨텍스트
        tcp::resolver resolver{ioc};                                  // DNS 리졸버
        auto const results = resolver.resolve(host, port);            // host:port → endpoint 목록

        websocket::stream<tcp::socket> ws{ioc};                       // WebSocket 스트림 생성
        net::connect(ws.next_layer(), results.begin(), results.end()); // TCP 연결 시도(성공 endpoint 선택)
        ws.handshake(host, "/");                                      // WebSocket 핸드셰이크(경로가 다르면 변경)
        ws.text(true);                                                // 텍스트 모드 명시(기본이지만 명시하면 가독성↑)
        std::cout << "Connected to " << host << ":" << port << "\n";  // 연결 로그

        constexpr auto kDelay = std::chrono::milliseconds(50);        // 전송 간격(약 50ms)

        // 채널별 set 전송 (50ms 간격)
        for (size_t i = 0; i < channels.size(); ++i) {                // 모든 채널 순회
            int ch = channels[i];                                     // 현재 채널
            std::string payload = std::string("{\"cmd\": \"set\", \"ch\":") // JSON payload 구성 시작
                                  + std::to_string(ch) + ", \"val\": "
                                  + (val ? "true" : "false") + "}";    // "val": true/false 붙이기
            ws.write(net::buffer(payload));                           // 서버로 텍스트 프레임 전송
            std::cout << "Sent: set ch=" << ch                        // 전송 로그 출력
                      << " val=" << (val ? "on" : "off") << "\n";
            if (i + 1 < channels.size())                              // 마지막 채널이 아니면
                std::this_thread::sleep_for(kDelay);                  // 50ms 대기 후 다음 채널 전송
        }

        std::this_thread::sleep_for(kDelay);                          // 마지막 set 이후 약간 대기(시리얼 처리 여유)
        ws.write(net::buffer(std::string(R"({"cmd": "get"})")));      // 최종 상태 조회(get) 요청

        beast::flat_buffer buffer;                                    // 수신 버퍼
        ws.read(buffer);                                              // 서버 응답 1개 읽기
        std::cout << "Received: "                                     // 응답 출력
                  << beast::make_printable(buffer.data()) << "\n";

        ws.close(websocket::close_code::normal);                      // 정상 종료(close 프레임 전송)
        return 0;                                                     // 성공 종료 코드
    } catch (const std::exception& e) {                               // 예외 포착(연결/IO/핸드셰이크 등)
        std::cerr << "Error: " << e.what() << "\n";                   // 에러 메시지 출력
        return 1;                                                     // 실패 종료 코드
    }
}
