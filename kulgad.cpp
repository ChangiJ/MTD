// ws_beast_opts_local_nospace_annotated.cpp                                                              // 파일 설명: HOST/PORT 인자 없음, 공백 처리 없는 간단 Beast WebSocket CLI (set/get + 범위/리스트 + 50ms)
// ------------------------------------------------------------------------------------------------------ // ───────────────────────────────────────────────────────────────────────────
// [Boost 설치/링크(마운트) 가이드]                                                                          // Boost 준비 방법
//  • Ubuntu/Debian:
//      sudo apt update && sudo apt install -y libboost-all-dev                                          // 설치
//      g++ -std=c++17 -O2 ws_beast_opts_local_nospace_annotated.cpp -o kulgad-cli -lboost_system -lpthread // 빌드
//  • macOS(Homebrew):
//      brew install boost                                                                                // 설치
//      g++ -std=c++17 -O2 ws_beast_opts_local_nospace_annotated.cpp -o kulgad-cli \                      // 빌드시 경로 지정 필요할 수 있음
//         -I/opt/homebrew/include -L/opt/homebrew/lib -lboost_system -lpthread                          // (Apple Silicon 예시)
//  • 커스텀 경로(수동 설치):
//      g++ -std=c++17 -O2 ws_beast_opts_local_nospace_annotated.cpp -o kulgad-cli \                      // -I/-L로 경로 지정
//         -I$HOME/boost/include -L$HOME/boost/lib -Wl,-rpath,$HOME/boost/lib \                          // 런타임 rpath 설정
//         -lboost_system -lpthread                                                                       // 링크
// ------------------------------------------------------------------------------------------------------ // ───────────────────────────────────────────────────────────────────────────
// Build: g++ -std=c++17 -O2 ws_beast_opts_local_nospace_annotated.cpp -o kulgad-cli -lboost_system -lpthread // 기본 빌드 명령
// Run  :                                                                                                // 사용 예시 (HOST/PORT 인자 없음)
//   ./kulgad-cli -s -on 100-231                                                                          // 채널 100~231 ON
//   ./kulgad-cli 10,2,3 -s -on                                                                           // 10,2,3 ON (옵션 순서 자유)
//   ./kulgad-cli 12,3,15 -off -s                                                                         // 12,3,15 OFF
//   ./kulgad-cli -g all                                                                                  // 전체 상태 조회
//   ./kulgad-cli -g 2-20                                                                                 // 2~20 상태 조회
//   # 중요: 채널 표기는 공백 없이만 허용 (예: 1,2,3,7-9). "1, 2" 같은 입력은 잘못된 형식으로 간주됨.                      //

#include <boost/beast/core.hpp>                                                                           // Beast: flat_buffer 등
#include <boost/beast/websocket.hpp>                                                                      // Beast: websocket::stream
#include <boost/asio/connect.hpp>                                                                         // ASIO: connect 유틸
#include <boost/asio/ip/tcp.hpp>                                                                          // ASIO: TCP 소켓/리졸버
#include <algorithm>                                                                                      // sort, unique
#include <cctype>                                                                                         // tolower, isdigit (※ isspace 사용 안 함)
#include <chrono>                                                                                         // 50ms 지연
#include <iostream>                                                                                       // 입출력
#include <string>                                                                                         // 문자열
#include <thread>                                                                                         // sleep_for
#include <vector>                                                                                         // 벡터

namespace beast = boost::beast;                                                                           // 네임스페이스 단축
namespace websocket = beast::websocket;                                                                   // 네임스페이스 단축
namespace net = boost::asio;                                                                              // 네임스페이스 단축
using tcp = boost::asio::ip::tcp;                                                                         // 타입 별칭

// ─────────────────────────── 보조 함수: 소문자 사본/숫자열 검사 ───────────────────────────
static std::string lower_copy(std::string s){                                                             // 문자열 소문자 사본 반환
    for(char& c: s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));                   // 문자별 소문자 변환
    return s;                                                                                             // 결과 반환
}
static bool is_all_digits(const std::string& s){                                                          // 전부 숫자인지 검사(빈 문자열 제외)
    if(s.empty()) return false;                                                                           // 빈 문자열이면 false
    return std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isdigit(c)!=0; });            // 모두 숫자면 true
}

// ─────────────────────────── 사용법 출력 ───────────────────────────
static void usage(){                                                                                      // 잘못된 인자 시 도움말
    std::cerr
      << "Usage:\n"                                                                                       // 제목
      << "  kulgad-cli [options] [channels]\n"                                                            // HOST/PORT 제거 -> 옵션/채널만
      << "Options:\n"                                                                                     // 옵션 설명
      << "  -s | set        : set 모드 (채널 상태 변경)\n"                                               // set
      << "  -g | get        : get 모드 (상태 조회)\n"                                                    // get
      << "  -on | on        : set 값 true\n"                                                             // on
      << "  -off| off       : set 값 false\n"                                                            // off
      << "Channels:\n"                                                                                    // 채널 지정 규칙
      << "  all | A-B | A,B,C | 혼합 가능. 반드시 공백 없이 입력 (예: 1,2,3,7-9)\n"                        // 공백 금지(코드로 처리하지 않음, 규칙만 안내)
      << "Notes:\n"                                                                                       // 참고
      << "  • 옵션/채널 순서는 자유(-s -on 10-20 / 10,2,3 -s -on / 12,3,15 -off -s 등)\n"               // 순서 자유
      << "  • 프로그램 종료 시 자동 get 하지 않음. get은 -g 옵션을 준 경우에만 수행.\n";                 // 자동 get 없음
}

// ─────────────────────────── 채널 파서: 공백 처리 없음, all/단일/범위/콤마 ───────────────────────────
static bool parse_channels_token(const std::string& token, std::vector<int>& out){                        // 채널 토큰 파서
    if(token.empty()){ std::cerr<<"Empty channel token.\n"; return false; }                                // 빈 토큰 금지
    std::string low = lower_copy(token);                                                                   // 소문자 사본
    if(low=="all"){                                                                                        // 'all'이면 전체 선택
        out.reserve(out.size()+256);                                                                       // 용량 예약
        for(int ch=0; ch<=255; ++ch) out.push_back(ch);                                                    // 0..255 모두 추가
        return true;                                                                                       // 성공
    }
    size_t pos=0;                                                                                          // 현재 인덱스
    while(pos<token.size()){                                                                               // 끝까지 처리
        size_t comma = token.find(',', pos);                                                               // 다음 콤마 위치
        std::string part = token.substr(pos, (comma==std::string::npos)?std::string::npos:comma-pos);      // 부분 토큰 (공백 제거/검사는 안 함)
        if(part.empty()){ std::cerr<<"Invalid channel list near ','\n"; return false; }                    // 빈 부분 금지
        size_t dash = part.find('-');                                                                      // 범위 구분자
        if(dash==std::string::npos){                                                                       // 단일 숫자
            if(!is_all_digits(part)){ std::cerr<<"Invalid channel: "<<part<<"\n"; return false; }          // 숫자 검증(공백이 섞여 있으면 실패)
            int ch = std::stoi(part);                                                                      // 정수 변환
            if(ch<0 || ch>255){ std::cerr<<"Channel out of range: "<<ch<<"\n"; return false; }             // 범위 체크
            out.push_back(ch);                                                                              // 추가
        }else{                                                                                              // 범위 A-B
            std::string a_str = part.substr(0,dash), b_str = part.substr(dash+1);                           // 좌/우 추출
            if(!is_all_digits(a_str) || !is_all_digits(b_str)){ std::cerr<<"Invalid range: "<<part<<"\n"; return false; } // 숫자 검증
            int a = std::stoi(a_str), b = std::stoi(b_str);                                                // 정수 변환
            if(a>b) std::swap(a,b);                                                                         // 정규화
            if(a<0 || b>255){ std::cerr<<"Range out of bounds: "<<part<<"\n"; return false; }              // 범위 체크
            for(int ch=a; ch<=b; ++ch) out.push_back(ch);                                                   // 범위 포함 추가
        }
        if(comma==std::string::npos) break;                                                                 // 마지막이면 종료
        pos = comma + 1;                                                                                    // 다음 부분으로
    }
    return true;                                                                                            // 성공
}

// ─────────────────────────── pins 배열만 단순 추출(JSON 라이트 파서) ───────────────────────────
static std::vector<bool> parse_pins_from_json(const std::string& js){                                      // {"pins":[true,false,...]} 단순 파싱
    std::vector<bool> pins;                                                                                 // 결과 컨테이너
    auto p = js.find("\"pins\""); if(p==std::string::npos) return pins;                                     // "pins" 키 탐색
    auto lb=js.find('[',p); if(lb==std::string::npos) return pins;                                          // '[' 찾기
    auto rb=js.find(']',lb); if(rb==std::string::npos) return pins;                                         // ']' 찾기
    std::string arr = js.substr(lb+1, rb-lb-1);                                                             // 배열 본문
    for(size_t i=0;i<arr.size();++i){                                                                       // 순차 스캔
        if(i+4<=arr.size() && arr.compare(i,4,"true")==0){ pins.push_back(true);  i+=3; }                   // true
        else if(i+5<=arr.size()&&arr.compare(i,5,"false")==0){ pins.push_back(false); i+=4; }               // false
    }
    return pins;                                                                                             // 결과 반환
}

// ─────────────────────────── 엔트리포인트 ───────────────────────────
int main(int argc, char** argv){                                                                            // 프로그램 진입점
    try{                                                                                                     // 예외 처리 시작
        // 고정 접속 대상                                                                                     //
        const std::string host = "localhost";                                                                // 고정 호스트
        const std::string port = "3001";                                                                     // 고정 포트

        // 파싱 상태                                                                                          //
        bool want_set=false, want_get=false;                                                                 // -s / -g 플래그
        bool have_val=false, val=false;                                                                      // -on/-off 지정 여부와 값
        std::vector<std::string> chanSpecs;                                                                  // 채널 스펙 토큰들

        if(argc<2){ usage(); return 1; }                                                                     // 인자 없으면 도움말

        // 토큰을 순서 무관으로 스캔 (공백 관련 처리는 전혀 하지 않음)                                             //
        for(int i=1;i<argc;++i){                                                                             // 1..N-1 순회
            std::string tok = argv[i];                                                                       // 현재 토큰 원문
            std::string low = lower_copy(tok);                                                               // 소문자 버전

            if(low=="-s" || low=="set"){ want_set=true; continue; }                                          // set 모드
            if(low=="-g" || low=="get"){ want_get=true; continue; }                                          // get 모드
            if(low=="-on"|| low=="on"){                                                                      // on
                if(have_val && val==false){ std::cerr<<"Conflicting options: -on and -off\n"; return 1; }    // 충돌 방지
                have_val=true; val=true; continue;                                                           // on 확정
            }
            if(low=="-off"|| low=="off"){                                                                    // off
                if(have_val && val==true){ std::cerr<<"Conflicting options: -on and -off\n"; return 1; }     // 충돌 방지
                have_val=true; val=false; continue;                                                          // off 확정
            }
            if(!tok.empty() && tok[0]=='-'){ usage(); return 1; }                                            // 알 수 없는 옵션 → 도움말
            chanSpecs.push_back(tok);                                                                         // 그 외는 채널 스펙으로 저장
        }

        // 필수 검사                                                                                          //
        if(!want_set && !want_get){ usage(); return 1; }                                                     // 최소 하나의 모드 필요
        if(chanSpecs.empty()){ std::cerr<<"No channels provided. Use e.g. 1,2,3 or 7-12 or all\n"; return 1; } // 채널 필수
        if(want_set && !have_val){ std::cerr<<"Missing value for set. Use -on or -off\n"; return 1; }        // set에는 -on/-off 필수

        // 채널 파싱 (공백 제거/검사 없음 — 사용자가 규칙을 준수해야 함)                                           //
        std::vector<int> channels;                                                                            // 최종 채널 목록
        for(const auto& spec: chanSpecs){                                                                     // 각 스펙 토큰
            if(!parse_channels_token(spec, channels)) return 1;                                               // 파싱 실패 시 종료
        }
        std::sort(channels.begin(), channels.end());                                                          // 정렬
        channels.erase(std::unique(channels.begin(), channels.end()), channels.end());                        // 중복 제거

        // 네트워크 연결 및 핸드셰이크                                                                          //
        net::io_context ioc;                                                                                  // ASIO 컨텍스트
        tcp::resolver resolver{ioc};                                                                          // DNS 리졸버
        auto eps = resolver.resolve(host, port);                                                              // host:port → 엔드포인트 목록
        websocket::stream<tcp::socket> ws{ioc};                                                               // WebSocket 스트림
        net::connect(ws.next_layer(), eps.begin(), eps.end());                                                // TCP 연결
        ws.handshake(host, "/");                                                                              // WebSocket 핸드셰이크(경로 변경 필요 시 수정)
        ws.text(true);                                                                                        // 텍스트 모드
        std::cout << "Connected to " << host << ":" << port << "\n";                                          // 연결 로그

        constexpr auto kDelay = std::chrono::milliseconds(50);                                                // 전송 간격(≈50ms)

        // SET 모드: 지정 채널에 대해 -on/-off 값을 적용                                                         //
        if(want_set){                                                                                         // -s / set 지정 시
            for(size_t idx=0; idx<channels.size(); ++idx){                                                    // 채널 순회
                int ch = channels[idx];                                                                       // 현재 채널
                std::string payload = std::string("{\"cmd\":\"set\",\"ch\":")                                 // JSON 구성
                                      + std::to_string(ch) + ",\"val\":" + (val?"true":"false") + "}";        // 값 포함
                ws.write(net::buffer(payload));                                                               // 동기 전송
                std::cout << "Sent: set ch="<<ch<<" val="<<(val?"on":"off")<<"\n";                            // 로그
                if(idx+1<channels.size()) std::this_thread::sleep_for(kDelay);                                // 다음 채널까지 50ms 대기
            }
            // ★ 자동 get 없음. 서버가 상태를 push하더라도 여기서 수신하지 않음.                                          //
        }

        // GET 모드: -g 지정 시에만 수행                                                                         //
        if(want_get){                                                                                         // -g / get 지정 시
            ws.write(net::buffer(std::string(R"({"cmd":"get"})")));                                           // 상태 요청
            beast::flat_buffer buf;                                                                           // 수신 버퍼
            ws.read(buf);                                                                                     // 응답 1개 수신(블로킹)
            std::string body = beast::buffers_to_string(buf.data());                                          // 문자열 변환
            auto pins = parse_pins_from_json(body);                                                           // pins 배열 추출
            if(pins.empty()){                                                                                 // pins 없으면
                std::cout << "Received (raw): " << body << "\n";                                              // 원문 출력
                std::cerr << "Warning: 'pins' array not found.\n";                                            // 경고
            }else{                                                                                            // pins 있으면
                std::cout << "Status:\n";                                                                     // 헤더
                int per_line=16, cnt=0;                                                                       // 1줄 16개
                for(int ch: channels){                                                                         // 요청 채널만 출력
                    bool in = (0<=ch && ch<(int)pins.size());                                                 // 인덱스 유효성
                    std::cout << ch << ":" << (in ? (pins[ch]?"on":"off") : "n/a")                            // on/off/n/a
                              << ((++cnt%per_line)?"  ":"\n");                                                // 간격/개행
                }
                if(cnt%per_line) std::cout << "\n";                                                           // 마지막 줄 개행 보정
            }
        }

        ws.close(websocket::close_code::normal);                                                              // 정상 종료(클로즈 프레임)
        return 0;                                                                                             // 성공 종료 코드

    }catch(const std::exception& e){                                                                          // 예외 처리
        std::cerr << "Error: " << e.what() << "\n";                                                           // 에러 메시지
        return 1;                                                                                             // 실패 종료 코드
    }
}
