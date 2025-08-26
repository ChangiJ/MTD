// ------------------------------------------------------------------------------------------------------ // ───────────────────────────────────────────────────────────────────────────
// [Boost 설치/링크(마운트) 가이드]                                                                          
//  • Ubuntu/Debian:
//      sudo apt update && sudo apt install -y libboost-all-dev                                          
//      g++ -std=c++17 -O2 kulgad.cpp -o kulgad-cli -lboost_system -lpthread // 빌드
//  • macOS(Homebrew):
//      brew install boost                                                                                
//      g++ -std=c++17 -O2 kulgad.cpp -o kulgad-cli \                      //
//         -I/opt/homebrew/include -L/opt/homebrew/lib -lboost_system -lpthread                         
//  • 커스텀 경로(수동 설치):
//      g++ -std=c++17 -O2 ws_beast_opts_local_nospace_annotated.cpp -o kulgad-cli \                      //
//         -I$HOME/boost/include -L$HOME/boost/lib -Wl,-rpath,$HOME/boost/lib \                          //
//         -lboost_system -lpthread                                                                       
// ------------------------------------------------------------------------------------------------------ // ───────────────────────────────────────────────────────────────────────────
// Build: g++ -std=c++17 -O2 kulgad.cpp -o kulgad-cli -lboost_system -lpthread // 기본 빌드 명령
// Run  :                                                                                                // 사용 예시 (HOST/PORT 인자 없음)
//   ./kulgad-cli -s -on 100-231                                                                          // 채널 100~231 ON
//   ./kulgad-cli 10,2,3 -s -on                                                                           // 10,2,3 ON (옵션 순서 자유)
//   ./kulgad-cli 12,3,15 -off -s                                                                         // 12,3,15 OFF
//   ./kulgad-cli -g all                                                                                  // 전체 상태 조회
//   ./kulgad-cli -g 2-20                                                                                 // 2~20 상태 조회
// ------------------------------------------------------------------------------------------------------ // ───────────────────────────────────────────────────────────────────────────

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

// ─────────────────────────── 보조 함수: 소문자 사본/숫자열 검사 ───────────────────────────
static std::string lower_copy(std::string s){
    for(char& c: s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static bool is_all_digits(const std::string& s){
    if(s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isdigit(c)!=0; });
}

// ─────────────────────────── 사용법 ───────────────────────────
static void usage(){
    std::cerr
      << "Usage:\n"
      << "  kulgad-cli [options] [channels]\n"
      << "Options:\n"
      << "  -s | --set        : set 모드 (채널 상태 변경)\n"
      << "  -g | --get        : get 모드 (상태 조회)\n"
      << "  -on | --on        : set 값 true\n"
      << "  -off| --off       : set 값 false\n"
      << "Channels:\n"
      << "  all | A-B | A,B,C | 혼합 가능. (예: 1,2,3,7-9)\n";
}

// ─────────────────────────── 채널 파서: all/단일/범위/콤마 ───────────────────────────
static bool parse_channels_token(const std::string& token, std::vector<int>& out){
    if(token.empty()){ std::cerr<<"Empty channel token.\n"; return false; }
    std::string low = lower_copy(token);
    if(low=="all"){														// all
        out.reserve(out.size()+256);
        for(int ch=0; ch<=255; ++ch) out.push_back(ch);
        return true;
    }
    size_t pos=0;
    while(pos<token.size()){
        size_t comma = token.find(',', pos);											// comma parsing
        std::string part = token.substr(pos, (comma==std::string::npos)?std::string::npos:comma-pos);
        if(part.empty()){ std::cerr<<"Invalid channel list near ','\n"; return false; }
        size_t dash = part.find('-');												// dash parsing
        if(dash==std::string::npos){
            if(!is_all_digits(part)){ std::cerr<<"Invalid channel: "<<part<<"\n"; return false; }
            int ch = std::stoi(part);
            if(ch<0 || ch>255){ std::cerr<<"Channel out of range: "<<ch<<"\n"; return false; }
            out.push_back(ch);
        }else{
            std::string a_str = part.substr(0,dash), b_str = part.substr(dash+1);
            if(!is_all_digits(a_str) || !is_all_digits(b_str)){ std::cerr<<"Invalid range: "<<part<<"\n"; return false; }
            int a = std::stoi(a_str), b = std::stoi(b_str);
            if(a>b) std::swap(a,b);
            if(a<0 || b>255){ std::cerr<<"Range out of bounds: "<<part<<"\n"; return false; }
            for(int ch=a; ch<=b; ++ch) out.push_back(ch);
        }
        if(comma==std::string::npos) break;
        pos = comma + 1;
    }
    return true;
}

// ─────────────────────────── pins 배열 추출 ───────────────────────────
static std::vector<bool> parse_pins_from_json(const std::string& js){
    std::vector<bool> pins;
    auto p  = js.find("\"pins\""); if(p==std::string::npos) return pins;
    auto lb = js.find('[', p);     if(lb==std::string::npos) return pins;
    auto rb = js.find(']', lb);    if(rb==std::string::npos) return pins;
    std::string arr = js.substr(lb + 1, rb - lb - 1);
    for(size_t i=0; i<arr.size(); ++i){
        if(i + 4 <= arr.size() && arr.compare(i, 4, "true")  == 0){ pins.push_back(true);  i += 3; }
        else if(i + 5 <= arr.size() && arr.compare(i, 5, "false") == 0){ pins.push_back(false); i += 4; }
    }
    return pins;
}

// ─────────────────────────── main함수 ───────────────────────────
int main(int argc, char** argv){
    try{
        const std::string host = "localhost";
        const std::string port = "3001";

        bool want_set=false, want_get=false;
        bool have_val=false, val=false;
        std::string chanSpec; 

        if(argc<2){ usage(); return 1; }

        for(int i=1;i<argc;++i){
            std::string tok = argv[i];
            std::string low = lower_copy(tok);
														// option: set, get, on/off
            if(low=="-s" || low=="--set"){ want_set=true; continue; }
            if(low=="-g" || low=="--get"){ want_get=true; continue; }
            if(low=="-on"|| low=="--on"){
                if(have_val && val==false){ std::cerr<<"Conflicting options: -on and -off\n"; return 1; }
                have_val=true; val=true; continue;
            }
            if(low=="-off"|| low=="--off"){
                if(have_val && val==true){ std::cerr<<"Conflicting options: -on and -off\n"; return 1; }
                have_val=true; val=false; continue;
            }
            if(!tok.empty() && tok[0]=='-'){ usage(); return 1; }
            if(!chanSpec.empty()){
                std::cerr << "Error: channels must be a single token without spaces (e.g. 1,3,5,7-9)\n";
                return 1;
            }
            chanSpec = tok;
        }

        if (want_get && !want_set && have_val) {
            std::cerr << "Error: -on/-off cannot be used with -g/--get when -s/--set is absent\n";
            return 1;
        }
        if(!want_set && !want_get){ usage(); return 1; }
        if(chanSpec.empty()){ std::cerr<<"No channels provided. Use e.g. 1,2,3 or 7-12 or all\n"; return 1; }
        if(want_set && !have_val){ std::cerr<<"Missing value for set. Use -on or -off\n"; return 1; }

        std::vector<int> channels;
        if(!parse_channels_token(chanSpec, channels)) return 1;
        std::sort(channels.begin(), channels.end());
        channels.erase(std::unique(channels.begin(), channels.end()), channels.end());

        net::io_context ioc;
        tcp::resolver resolver{ioc};
        auto eps = resolver.resolve(host, port);
        websocket::stream<tcp::socket> ws{ioc};
        net::connect(ws.next_layer(), eps.begin(), eps.end());
        ws.handshake(host, "/");
        ws.text(true);
        std::cout << "Connected to " << host << ":" << port << "\n";

        constexpr auto kDelay = std::chrono::milliseconds(50);

        if(want_set){												// set
            for(size_t idx=0; idx<channels.size(); ++idx){
                int ch = channels[idx];
                std::string payload = std::string("{\"cmd\":\"set\",\"ch\":")
                                      + std::to_string(ch) + ",\"val\":" + (val?"true":"false") + "}";
                ws.write(net::buffer(payload));
                std::cout << "Sent: set ch="<<ch<<" val="<<(val?"on":"off")<<"\n";
                if(idx+1<channels.size()) std::this_thread::sleep_for(kDelay);
            }
        }

        if(want_get){												// get
            ws.write(net::buffer(std::string(R"({"cmd":"get"})")));
            beast::flat_buffer buf;
            ws.read(buf);											// read buffer
            std::string body = beast::buffers_to_string(buf.data());
            auto pins = parse_pins_from_json(body);
            if(pins.empty()){
                std::cout << "Received (raw): " << body << "\n";
                std::cerr << "Warning: 'pins' array not found.\n";
            }else{
                std::cout << "Status:\n";
                int per_line=16, cnt=0;
                for(int ch: channels){
                    bool in = (0<=ch && ch<(int)pins.size());
                    std::cout << ch << ":" << (in ? (pins[ch]?"on":"off") : "n/a")
                              << ((++cnt%per_line)?"  ":"\n");
                }
                if(cnt%per_line) std::cout << "\n";
            }
        }

        ws.close(websocket::close_code::normal);
        return 0;

    }catch(const std::exception& e){
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

