// ws_beast_simple_annotated.cpp                                                                       // 파일 설명: 단순화한 Beast WebSocket CLI (set/get + all/범위/리스트 + 50ms 간격)
// ---------------------------------------------------------------------------------------------------- // ───────────────────────────────────────────────────────────────────────────
// [Boost 마운트(설치/링크) 방법]                                                                          // Boost 설치/링크 안내
//  • Ubuntu/Debian:                                                                                   // 리눅스 계열(데비안/우분투)
//      sudo apt update && sudo apt install -y libboost-all-dev                                       // 패키지로 설치
//      g++ -std=c++17 -O2 ws_beast_simple_annotated.cpp -o kulgad-cli -lboost_system -lpthread        // 컴파일/링크
//  • macOS(Homebrew):                                                                                 // 맥OS
//      brew install boost                                                                             // 설치
//      g++ -std=c++17 -O2 ws_beast_simple_annotated.cpp -o kulgad-cli \                               // 컴파일 시 헤더/라이브러리 경로 필요할 수 있음
//         -I/opt/homebrew/include -L/opt/homebrew/lib -lboost_system -lpthread                        // (Apple Silicon 기준 기본 경로 예시)
//  • 커스텀 경로(직접 빌드/설치했을 때):                                                                  // 수동 설치한 경우
//      g++ -std=c++17 -O2 ws_beast_simple_annotated.cpp -o kulgad-cli \                               // -I/-L로 경로 지정
//         -I$HOME/boost/include -L$HOME/boost/lib -Wl,-rpath,$HOME/boost/lib \                        // 런타임 검색(rpath)도 함께
//         -lboost_system -lpthread                                                                     // 링크 라이브러리
// ---------------------------------------------------------------------------------------------------- // ───────────────────────────────────────────────────────────────────────────
// Build: g++ -std=c++17 -O2 ws_beast_simple_annotated.cpp -o kulgad-cli -lboost_system -lpthread      // 기본 빌드 명령
// Run  :                                                                                               // 실행 예시
//   ./kulgad-cli set 3 on                                                                              // 채널 3 on
//   ./kulgad-cli set 0-4,7,10-12 off                                                                    // 범위/리스트 혼용
//   ./kulgad-cli set all on                                                                             // 전체 on
//   ./kulgad-cli get all                                                                                // 전체 상태 조회
//   ./kulgad-cli get 2-20                                                                               // 2~20 상태 조회
//   ./kulgad-cli 210.119.41.68 3001 set 5,9 off                                                         // 원격 호스트/포트 지정

#include <boost/beast/core.hpp>                                                                          // Beast 기본 타입(flat_buffer 등)
#include <boost/beast/websocket.hpp>                                                                     // Beast WebSocket API
#include <boost/asio/connect.hpp>                                                                        // ASIO 연결 유틸
#include <boost/asio/ip/tcp.hpp>                                                                         // ASIO TCP
#include <algorithm>                                                                                     // sort, unique, remove_if
#include <cctype>                                                                                        // tolower, isspace
#include <chrono>                                                                                        // 50ms 지연
#include <iostream>                                                                                      // 입출력
#include <string>                                                                                        // 문자열
#include <thread>                                                                                        // sleep_for
#include <vector>                                                                                        // 채널 벡터

namespace beast = boost::beast;                                                                          // 네임스페이스 단축
namespace websocket = beast::websocket;                                                                  // 네임스페이스 단축
namespace net = boost::asio;                                                                             // 네임스페이스 단축
using tcp = boost::asio::ip::tcp;                                                                        // 타입 별칭

enum class Mode { SET, GET };                                                                            // 동작 모드 구분

// 간단 사용법 출력
static void usage() {                                                                                    // 잘못된 인자일 때 도움말 표시
    std::cerr                                                                                           //
      << "Usage:\n"                                                                                     //
      << "  kulgad-cli [host] [port] set <channels|all> <on|off>\n"                                     // set 형식
      << "  kulgad-cli [host] [port] get <channels|all>\n"                                              // get 형식
      << "  <channels>: 0..255, comma and range supported (e.g., 0-4,7,10-12)\n";                       // 채널 지정 규칙
}

// 채널 문자열 파싱: "all" 또는 "3,7,10-12" 등 → 정수 벡터(0..255, 중복 제거)
static bool parse_channels(std::string s, std::vector<int>& out) {                                       // 채널 파서(간결)
    s.erase(std::remove_if(s.begin(), s.end(),                                                           // 모든 공백 제거
                           [](unsigned char c){ return std::isspace(c); }),
            s.end());                                                                                    //
    if (s.empty()) return false;                                                                         // 비어 있으면 실패

    std::string lower = s;                                                                               // 소문자 사본
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });                                      // 소문자화
    if (lower == "all" || lower.find("all") != std::string::npos) {                                      // 'all' 포함 시
        out.reserve(256);                                                                                // 전체 채널 준비
        for (int ch = 0; ch <= 255; ++ch) out.push_back(ch);                                             // 0~255 추가
    } else {                                                                                             // 목록/범위 파싱
        size_t pos = 0;                                                                                  // 현재 위치
        auto add = [&](int ch){                                                                          // 단일 숫자 추가 헬퍼
            if (ch < 0 || ch > 255) { std::cerr << "Channel out of range: " << ch << "\n"; return false; }
            out.push_back(ch); return true;                                                              //
        };
        while (pos < s.size()) {                                                                         // 끝까지
            size_t comma = s.find(',', pos);                                                             // 다음 콤마
            std::string tok = s.substr(pos, (comma==std::string::npos)?std::string::npos:comma-pos);     // 토큰 추출
            if (!tok.empty()) {                                                                          // 비어있지 않으면
                size_t dash = tok.find('-');                                                             // 범위 구분자
                if (dash == std::string::npos) {                                                         // 단일 숫자
                    try { if (!add(std::stoi(tok))) return false; }                                      // 정수 변환+검증
                    catch (...) { std::cerr << "Invalid channel: " << tok << "\n"; return false; }       // 실패
                } else {                                                                                 // 범위 a-b
                    std::string a_str = tok.substr(0, dash), b_str = tok.substr(dash+1);                 // 좌/우
                    if (a_str.empty() || b_str.empty()) { std::cerr << "Invalid range: " << tok << "\n"; return false; }
                    int a, b;                                                                            //
                    try { a = std::stoi(a_str); b = std::stoi(b_str); }                                  // 정수 변환
                    catch (...) { std::cerr << "Invalid range: " << tok << "\n"; return false; }         // 실패
                    if (a > b) std::swap(a, b);                                                          // a<=b로 정규화
                    if (a < 0 || b > 255) { std::cerr << "Range out of bounds: " << tok << "\n"; return false; }
                    for (int ch=a; ch<=b; ++ch) out.push_back(ch);                                       // 전체 추가
                }
            }
            if (comma == std::string::npos) break;                                                       // 마지막이면 종료
            pos = comma + 1;                                                                             // 다음 토큰
        }
        if (out.empty()) { std::cerr << "No valid channels parsed\n"; return false; }                    // 결과 없음
    }
    std::sort(out.begin(), out.end());                                                                   // 정렬
    out.erase(std::unique(out.begin(), out.end()), out.end());                                           // 중복 제거
    return true;                                                                                         // 성공
}

// 인자 파싱: [host] [port] (set|get) ...
static bool parse_args(int argc, char** argv,                                                             // 인자들
                       std::string& host, std::string& port,                                             // 결과: host/port
                       Mode& mode, std::vector<int>& channels, bool& val)                                // 결과: 모드/채널/상태
{
    host = "localhost";                                                                                  // 기본 host
    port = "3001";                                                                                       // 기본 port
    if (argc < 3) { usage(); return false; }                                                             // 최소 검증

    int idx = 1;                                                                                         // 명령 위치
    std::string cmd = argv[1];                                                                           // 첫 토큰
    if (cmd != "set" && cmd != "get") {                                                                  // set/get 아니면
        if (argc < 5) { usage(); return false; }                                                         // host/port 포함 최소 개수 확인
        host = argv[1];                                                                                  // host 지정
        port = argv[2];                                                                                  // port 지정
        idx  = 3;                                                                                        // 명령 인덱스
        cmd  = argv[idx];                                                                                // 명령 추출
    }

    if (cmd == "set") {                                                                                  // SET 모드
        if (argc < idx + 3) { usage(); return false; }                                                   // set <channels> <on|off>
        std::string chanSpec;                                                                            // 채널 사양 문자열
        for (int i = idx + 1; i < argc - 1; ++i) {                                                       // 상태 앞까지 모두 결합
            if (!chanSpec.empty()) chanSpec.push_back(' ');                                              // 공백(나중 제거)
            chanSpec += argv[i];                                                                         // 누적
        }
        if (!parse_channels(chanSpec, channels)) return false;                                           // 채널 파싱
        std::string state = argv[argc - 1];                                                              // 마지막: on/off
        std::transform(state.begin(), state.end(), state.begin(),
                       [](unsigned char c){ return std::tolower(c); });                                  // 소문자화
        if      (state=="on"  || state=="true"  || state=="1") val = true;                               // on 계열
        else if (state=="off" || state=="false" || state=="0") val = false;                              // off 계열
        else { std::cerr << "State must be on/off\n"; return false; }                                    // 그 외 오류
        mode = Mode::SET;                                                                                // 모드 설정
        return true;                                                                                     // 성공
    }

    if (cmd == "get") {                                                                                  // GET 모드
        if (argc < idx + 2) { usage(); return false; }                                                   // get <channels>
        std::string chanSpec;                                                                            // 채널 사양 문자열
        for (int i = idx + 1; i < argc; ++i) {                                                           // 끝까지 결합
            if (!chanSpec.empty()) chanSpec.push_back(' ');                                              // 공백(나중 제거)
            chanSpec += argv[i];                                                                         // 누적
        }
        if (!parse_channels(chanSpec, channels)) return false;                                           // 채널 파싱
        mode = Mode::GET;                                                                                // 모드 설정
        return true;                                                                                     // 성공
    }

    usage();                                                                                             // 기타는 도움말
    return false;                                                                                        // 실패
}

// 아주 단순한 JSON 파서: {"pins":[true,false,...]} 에서 pins 배열만 추출
static std::vector<bool> parse_pins_from_json(const std::string& js) {                                   // 정식 JSON 파서 대신 간단 문자열 스캔
    std::vector<bool> pins;                                                                               // 결과 담을 벡터
    auto p  = js.find("\"pins\"");                                                                        // "pins" 키 위치
    if (p == std::string::npos) return pins;                                                              // 없으면 빈 벡터
    auto lb = js.find('[', p);                                                                            // 좌 대괄호
    if (lb == std::string::npos) return pins;                                                             // 실패
    auto rb = js.find(']', lb);                                                                           // 우 대괄호
    if (rb == std::string::npos) return pins;                                                             // 실패
    std::string arr = js.substr(lb + 1, rb - lb - 1);                                                     // 배열 내용만 추출
    for (size_t i = 0; i < arr.size(); ++i) {                                                             // 순회하며
        if (i + 4 <= arr.size() && arr.compare(i, 4, "true") == 0) { pins.push_back(true);  i += 3; }     // true
        else if (i + 5 <= arr.size() && arr.compare(i, 5, "false")==0){ pins.push_back(false); i += 4; }  // false
    }
    return pins;                                                                                          // 결과 반환
}

int main(int argc, char** argv) {                                                                         // 진입점
    try {                                                                                                 // 예외 처리
        std::string host, port;                                                                           // host/port
        Mode mode;                                                                                        // 모드(SET/GET)
        std::vector<int> channels;                                                                        // 채널 목록
        bool val = false;                                                                                 // SET일 때 on/off

        if (!parse_args(argc, argv, host, port, mode, channels, val)) return 1;                           // 인자 파싱

        net::io_context ioc;                                                                              // ASIO 컨텍스트
        tcp::resolver resolver{ioc};                                                                      // DNS 리졸버
        auto endpoints = resolver.resolve(host, port);                                                    // host:port → endpoints

        websocket::stream<tcp::socket> ws{ioc};                                                           // WebSocket 스트림
        net::connect(ws.next_layer(), endpoints.begin(), endpoints.end());                                // TCP 연결
        ws.handshake(host, "/");                                                                          // WS 핸드셰이크(경로 필요시 변경)
        ws.text(true);                                                                                    // 텍스트 모드
        std::cout << "Connected to " << host << ":" << port << "\n";                                      // 연결 로그

        constexpr auto kDelay = std::chrono::milliseconds(50);                                            // 전송 간격(≈50ms)

        if (mode == Mode::SET) {                                                                          // SET 처리
            for (size_t i = 0; i < channels.size(); ++i) {                                                // 모든 채널에 대해
                int ch = channels[i];                                                                     // 채널 번호
                std::string payload = std::string("{\"cmd\":\"set\",\"ch\":")                             // JSON
                                      + std::to_string(ch) + ",\"val\":" + (val ? "true" : "false") + "}";// 구성
                ws.write(net::buffer(payload));                                                           // 전송
                std::cout << "Sent: set ch=" << ch << " val=" << (val?"on":"off") << "\n";                // 로그
                if (i + 1 < channels.size()) std::this_thread::sleep_for(kDelay);                         // 간격 대기
            }
            std::this_thread::sleep_for(kDelay);                                                          // 마지막 set 후 여유
        }

        ws.write(net::buffer(std::string(R"({"cmd":"get"})")));                                           // GET 요청(SET 뒤에도 최종 조회)
        beast::flat_buffer buf;                                                                           // 수신 버퍼
        ws.read(buf);                                                                                     // 응답 1개 수신
        std::string body = beast::buffers_to_string(buf.data());                                          // 문자열로 변환
        auto pins = parse_pins_from_json(body);                                                           // pins 배열 파싱

        if (pins.empty()) {                                                                               // pins 배열이 없으면
            std::cout << "Received (raw): " << body << "\n";                                              // 원문 출력
            std::cerr << "Warning: 'pins' array not found.\n";                                            // 경고
        } else {                                                                                          // pins 있음
            std::cout << "Status:\n";                                                                     // 헤더
            int per_line = 16, cnt = 0;                                                                   // 한 줄당 16개 출력
            for (int ch : channels) {                                                                     // 요청 채널만 출력
                bool in = (0 <= ch && ch < (int)pins.size());                                             // 인덱스 유효?
                std::cout << ch << ":" << (in ? (pins[ch]?"on":"off") : "n/a")                            // 상태 문자열
                          << ((++cnt % per_line) ? "  " : "\n");                                          // 간격/개행
            }
            if (cnt % per_line) std::cout << "\n";                                                        // 마지막 줄 개행 보정
        }

        ws.close(websocket::close_code::normal);                                                          // 정상 종료
        return 0;                                                                                         // 성공 코드
    } catch (const std::exception& e) {                                                                   // 예외
        std::cerr << "Error: " << e.what() << "\n";                                                       // 에러 출력
        return 1;                                                                                         // 실패 코드
    }
}
