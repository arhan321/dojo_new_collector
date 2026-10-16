// RMS Yaskawa Collector v2: read-only YMConnect, local Laravel v1 API.
// Mock mode is a full offline-data pipeline. REAL extensions require matching YMConnect.h.
// Never controls servos, movement, jobs, IO writes, or safety functions.
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
#if defined(RMS_WITH_YMCONNECT) && RMS_WITH_YMCONNECT
#include "YMConnect.h"
#endif

namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
static std::atomic<bool> running{true};
static std::mutex logMutex;
static const std::array<std::string,6> AXES{{"S","L","U","R","B","T"}};
static void onSignal(int){running=false;}
static void log(const std::string& s){std::lock_guard<std::mutex> lock(logMutex);std::cout<<s<<std::endl;}
static std::string trim(std::string s){auto a=s.find_first_not_of(" \t\r\n");if(a==std::string::npos)return {};auto b=s.find_last_not_of(" \t\r\n");return s.substr(a,b-a+1);}
static std::string quote(const std::string&s){std::ostringstream o;o<<'"';for(unsigned char c:s){switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;default:if(c<0x20){o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec<<std::setfill(' ');}else o<<char(c);}}o<<'"';return o.str();}
static std::string number(double v){if(!std::isfinite(v))throw std::runtime_error("Non-finite numeric SDK data");std::ostringstream o;o<<std::setprecision(15)<<v;return o.str();}
class Json{
 std::ostringstream out;bool first=true;
 public:
 void raw(const std::string&k,const std::string&v){if(!first)out<<',';first=false;out<<quote(k)<<':'<<v;}
 void str(const std::string&k,const std::string&v){raw(k,quote(v));}
 void flag(const std::string&k,bool b){raw(k,b?"true":"false");}
 void num(const std::string&k,long long v){raw(k,std::to_string(v));}
 void num(const std::string&k,int v){raw(k,std::to_string(v));}
 void num(const std::string&k,double v){raw(k,number(v));}
 void null(const std::string&k){raw(k,"null");}
 std::string dump()const{return "{"+out.str()+"}";}
};
static std::string timestamp(){auto n=std::chrono::system_clock::now();auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(n.time_since_epoch())%1000;auto tt=std::chrono::system_clock::to_time_t(n);std::tm tm{};
#if defined(_WIN32)
 gmtime_s(&tm,&tt);
#else
 gmtime_r(&tt,&tm);
#endif
 std::ostringstream o;o<<std::put_time(&tm,"%Y-%m-%dT%H:%M:%S")<<'.'<<std::setfill('0')<<std::setw(3)<<ms.count()<<'Z';return o.str();}
static std::string uuid(){std::random_device rd;std::mt19937_64 g((uint64_t(rd())<<32)^rd());std::array<unsigned char,16> x{};for(auto&v:x)v=static_cast<unsigned char>(g()&255);x[6]=(x[6]&15)|64;x[8]=(x[8]&63)|128;std::ostringstream o;for(int i=0;i<16;i++){if(i==4||i==6||i==8||i==10)o<<'-';o<<std::hex<<std::setfill('0')<<std::setw(2)<<int(x[i]);}return o.str();}
static std::string axesJson(const std::array<double,6>& a){Json j;for(int i=0;i<6;i++)j.num(AXES[i],a[i]);return j.dump();}
static int integer(const std::string&k,const std::string&s,int lo,int hi){size_t p=0;long n=std::stol(s,&p);if(p!=s.size()||n<lo||n>hi)throw std::runtime_error("Invalid "+k+": "+s);return int(n);}
static std::vector<std::string> split(const std::string&s,char delimiter){std::vector<std::string>v;std::stringstream in(s);std::string a;while(std::getline(in,a,delimiter))v.push_back(trim(a));return v;}
struct Device{int id=0;std::string ip;std::string group="R1";std::vector<int> io;};
struct Config{
 std::string base="http://127.0.0.1:8080";std::string token="./collector.token";std::string spool="./spool";
 int telemetryMs=5000,operatingS=60,alarmS=30,extrasS=10,timeoutS=5,retryLimit=20000;
 std::vector<Device> devices;
};
static Config configFrom(const std::string&path){std::ifstream f(path);if(!f)throw std::runtime_error("Missing config: "+path);Config c;std::string l;int line=0;while(std::getline(f,l)){++line;l=trim(l);if(l.empty()||l[0]=='#')continue;auto p=l.find('=');if(p==std::string::npos)throw std::runtime_error("Expected key=value at "+std::to_string(line));auto k=trim(l.substr(0,p)),v=trim(l.substr(p+1));
 if(k=="api_base")c.base=v;else if(k=="token_file")c.token=v;else if(k=="spool_dir")c.spool=v;
 else if(k=="telemetry_ms")c.telemetryMs=integer(k,v,1000,3600000);
 else if(k=="operating_every_s")c.operatingS=integer(k,v,5,86400);
 else if(k=="alarms_every_s")c.alarmS=integer(k,v,5,86400);
 else if(k=="extras_every_s")c.extrasS=integer(k,v,5,86400);
 else if(k=="http_timeout_s")c.timeoutS=integer(k,v,1,120);
 else if(k=="spool_max")c.retryLimit=integer(k,v,100,200000);
 else if(k=="robot") { // robot=DB_ID,CONTROLLER_IP,R1,IO_ADDRESS;IO_ADDRESS
  auto a=split(v,',');if(a.size()<2||a.size()>4)throw std::runtime_error("robot config requires id,ip[,group[,io;io]]");
  Device d;d.id=integer("robot_id",a[0],1,1000000);d.ip=a[1];if(a.size()>=3)d.group=a[2];
  if(d.ip.empty())throw std::runtime_error("Robot IP is empty");
  if(d.group!="R1")throw std::runtime_error("This version is validated only for group R1; configure a separate validated adapter for "+d.group);
  if(a.size()==4&&!a[3].empty())for(auto &x:split(a[3],';'))d.io.push_back(integer("IO address",x,1,99999));
  if(d.io.size()>24)throw std::runtime_error("Maximum 24 IO byte addresses per robot");
  for(auto &old:c.devices)if(old.id==d.id||old.ip==d.ip)throw std::runtime_error("Duplicate robot ID/IP");
  c.devices.push_back(std::move(d));
 }else throw std::runtime_error("Unknown config key: "+k);
 }
 if(c.base.rfind("http://",0)!=0||c.base.find_first_of("?#")!=std::string::npos)throw std::runtime_error("Use a trusted local/LAN http:// API base URL, no query string");
 while(c.base.size()>7&&c.base.back()=='/')c.base.pop_back();
 if(c.devices.empty())throw std::runtime_error("Add at least one robot= entry");
 if(c.devices.size()>8)throw std::runtime_error("Pilot max 8 robots; validate controller load before scaling");
 return c;}
static std::string readToken(const fs::path&path){std::ifstream f(path);std::string token;if(!f||!std::getline(f,token))throw std::runtime_error("Missing collector token file: "+path.string());token=trim(token);if(token.empty()||token.find_first_of(" \r\n\t")!=std::string::npos)throw std::runtime_error("Bad token file");return token;}
static size_t sink(char*p,size_t n,size_t m,void*data){auto&s=*static_cast<std::string*>(data);size_t bytes=n*m;if(s.size()<4096)s.append(p,std::min(bytes,4096-s.size()));return bytes;}
struct HttpResult{CURLcode error=CURLE_OK;long status=0;std::string body;bool ok()const{return error==CURLE_OK&&(status==200||status==201);}bool fatal()const{return error==CURLE_OK&&(status==400||status==401||status==403||status==404||status==405||status==422);}std::string summary()const{return error==CURLE_OK?std::to_string(status):curl_easy_strerror(error);}};
class Http{
 const Config& c;std::string bearer;
 public:Http(const Config& cfg,std::string token):c(cfg),bearer(std::move(token)){}
 HttpResult run(const std::string&endpoint,const std::string&payload={},bool isPost=true){CURL* h=curl_easy_init();if(!h)throw std::runtime_error("curl_easy_init failed");HttpResult r;std::string url=c.base+endpoint;curl_slist*headers=nullptr;
  headers=curl_slist_append(headers,"Accept: application/json");if(isPost){headers=curl_slist_append(headers,"Content-Type: application/json");auto auth="Authorization: Bearer "+bearer;headers=curl_slist_append(headers,auth.c_str());curl_easy_setopt(h,CURLOPT_POST,1L);curl_easy_setopt(h,CURLOPT_POSTFIELDS,payload.c_str());curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE_LARGE,curl_off_t(payload.size()));}
  curl_easy_setopt(h,CURLOPT_URL,url.c_str());curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,3L);curl_easy_setopt(h,CURLOPT_TIMEOUT,long(c.timeoutS));curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,sink);curl_easy_setopt(h,CURLOPT_WRITEDATA,&r.body);r.error=curl_easy_perform(h);if(r.error==CURLE_OK)curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&r.status);curl_slist_free_all(headers);curl_easy_cleanup(h);return r;
 }
};
static bool endpointAllowed(const std::string&ep){return ep=="/api/v1/collector/telemetry"||ep=="/api/v1/collector/operating-times"||ep=="/api/v1/collector/alarms/snapshots"||ep=="/api/v1/collector/connections";}
// Each robot runs with its own dispatcher/spool directory so threads never race on queue filenames.
class Dispatcher{
 Config c;Device device;bool dry;std::unique_ptr<Http> http;fs::path dir;uint64_t counter=0;
 void spool(const std::string&ep,const std::string&json){fs::create_directories(dir);size_t cnt=0;for(auto&it:fs::directory_iterator(dir))if(it.is_regular_file()&&it.path().extension()==".queue")cnt++;if(cnt>=size_t(c.retryLimit))throw std::runtime_error("Queue full; collector stops instead of silently dropping data");
  auto t=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();fs::path name=dir/(std::to_string(t)+"_"+std::to_string(counter++)+".queue");fs::path tmp=name.string()+".tmp";{std::ofstream out(tmp,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Cannot write spool");out<<ep<<'\n'<<json<<'\n';out.flush();if(!out)throw std::runtime_error("Spool write failed");}fs::rename(tmp,name);}
 public:Dispatcher(const Config&config,const Device&d,bool isDry,const std::string&token):c(config),device(d),dry(isDry),dir(fs::path(c.spool)/std::to_string(d.id)){if(!dry){fs::create_directories(dir);http=std::make_unique<Http>(c,token);}}
 void post(const std::string&ep,const std::string&json){if(!endpointAllowed(ep))throw std::runtime_error("Unsafe endpoint");if(dry){log("[DRY id="+std::to_string(device.id)+"] POST "+ep+" "+json);return;}auto r=http->run(ep,json);if(r.ok()){log("[API "+std::to_string(device.id)+"] "+ep+" => "+r.summary());return;}spool(ep,json);log("[SPOOL "+std::to_string(device.id)+"] "+ep+" failed "+r.summary()+" "+r.body.substr(0,160));if(r.fatal())throw std::runtime_error("Laravel rejected POST (auth/permission/validation). Fix API before continuing.");}
 void flush(){if(dry)return;std::vector<fs::path>files;for(auto&p:fs::directory_iterator(dir))if(p.is_regular_file()&&p.path().extension()==".queue")files.push_back(p.path());std::sort(files.begin(),files.end());size_t n=0;for(auto&path:files){if(n++>=10)break;std::ifstream in(path);std::string ep,json;if(!std::getline(in,ep)||!std::getline(in,json)||!endpointAllowed(ep))throw std::runtime_error("Invalid queue file: "+path.string());auto r=http->run(ep,json);if(r.ok()){fs::remove(path);log("[REPLAY "+std::to_string(device.id)+"] HTTP "+r.summary());}else {if(r.fatal())throw std::runtime_error("Queue replay blocked by permanent API rejection: "+r.summary());break;}}}
};
struct Reading{bool ok=false;int code=-1;std::string error="unavailable",raw;};
struct RobotSample{
 Reading pos,torque,operating,alarm,state,job,io,alarmHistory;
 std::array<double,6> pulses{},torques{};
 std::optional<int64_t> operatingSeconds;
 std::optional<int> activeAlarms;
 std::optional<std::string> jobName;
 std::string stateJson="null",jobJson="null",ioJson="null",alarmHistoryJson="null";
};
// Stream-based SDK diagnostics are not guaranteed to be machine-readable structured fields.
template<typename T,typename=void>struct Streamable:std::false_type{};
template<typename T>struct Streamable<T,std::void_t<decltype(std::declval<std::ostream&>()<<std::declval<const T&>())>>:std::true_type{};
template<typename T>static std::string streamIfPossible(const T&x){if constexpr(Streamable<T>::value){std::ostringstream out;out<<x;return out.str();}else return "type not streamable in installed SDK";}
// Optional SDK method detection: supports multiple installed header versions without
// assuming nonexistent typedefs. Only unambiguous, single-output signatures are invoked.
#if defined(RMS_WITH_YMCONNECT) && RMS_WITH_YMCONNECT
template<typename T,typename=void>struct HasReadState:std::false_type{};
template<typename T>struct HasReadState<T,std::void_t<decltype(&T::ReadState)>>:std::true_type{};
template<typename T,typename=void>struct HasJobInfo:std::false_type{};
template<typename T>struct HasJobInfo<T,std::void_t<decltype(&T::GetExecutingJobInformation)>>:std::true_type{};
template<typename T,typename=void>struct HasAlarmHistory:std::false_type{};
template<typename T>struct HasAlarmHistory<T,std::void_t<decltype(&T::GetAlarmHistory)>>:std::true_type{};
template<typename T>struct MethodArgs;
template<typename Ret,typename C,typename... Args>struct MethodArgs<Ret(C::*)(Args...)>{using ArgsTuple=std::tuple<Args...>;static constexpr size_t size=sizeof...(Args);};
template<typename Ret,typename C,typename... Args>struct MethodArgs<Ret(C::*)(Args...) const>{using ArgsTuple=std::tuple<Args...>;static constexpr size_t size=sizeof...(Args);};
// Supports methods of the form StatusInfo ReadX(OutputData &), not guesses.
template<typename Interface,typename Fn>
static std::pair<Reading,std::string> readSingleOutput(Interface*iface,Fn method){
 using Traits=MethodArgs<Fn>;
 if constexpr(Traits::size==1){
  using Arg=std::tuple_element_t<0,typename Traits::ArgsTuple>;
  if constexpr(std::is_lvalue_reference_v<Arg> && !std::is_const_v<std::remove_reference_t<Arg>>){
   using Out=std::remove_reference_t<Arg>;
   Out data{};auto status=(iface->*method)(data);
   bool ok=status.StatusCode==0;
   return {{ok,int(status.StatusCode),ok?"":"YMConnect read returned error",ok?streamIfPossible(data):""},ok?streamIfPossible(data):""};
  }
 }
 return {{false,-1,"SDK method output signature not recognized", ""},""};
}
// ReadState / Job output fields vary by SDK version. Retain OEM text as evidence;
// never pretend text alone is an approved structured status or job identifier.
template<typename StatusInterface>static std::pair<Reading,std::string> tryControllerState(StatusInterface*iface){
 if constexpr(HasReadState<StatusInterface>::value)return readSingleOutput(iface,&StatusInterface::ReadState);
 else return {{false,-1,"ReadState unavailable in installed YMConnect SDK", ""},""};
}
template<typename JobInterfaceType>static std::pair<Reading,std::string> tryJob(JobInterfaceType*iface){
 if constexpr(HasJobInfo<JobInterfaceType>::value)return readSingleOutput(iface,&JobInterfaceType::GetExecutingJobInformation);
 else return {{false,-1,"GetExecutingJobInformation unavailable in installed YMConnect SDK", ""},""};
}
template<typename FaultInterfaceType>static std::pair<Reading,std::string> tryHistory(FaultInterfaceType*iface){
 if constexpr(HasAlarmHistory<FaultInterfaceType>::value)return readSingleOutput(iface,&FaultInterfaceType::GetAlarmHistory);
 else return {{false,-1,"GetAlarmHistory unavailable or overloaded in installed YMConnect SDK", ""},""};
}
#endif
class Reader{public:virtual ~Reader()=default;virtual bool connect()=0;virtual void disconnect()=0;virtual bool connected()const=0;virtual RobotSample collect(long long sequence,bool extra,bool op,bool alarms)=0;};
class MockReader:public Reader{
 bool up=false;Device d;
 public:explicit MockReader(Device device):d(std::move(device)){}bool connect()override{up=true;return true;}void disconnect()override{up=false;}bool connected()const override{return up;}
 RobotSample collect(long long sequence,bool extra,bool op,bool alarms)override{RobotSample s;s.pos={true,0,"","mock positions"};s.torque={true,0,"","mock torque"};
  for(int i=0;i<6;i++){s.pulses[i]=100000+i*12000+sequence*5;s.torques[i]=9+i*1.1+0.4*std::sin(double(sequence)*0.25+i);}
  if(op){s.operating={true,0,"","SIMULATED seconds"};s.operatingSeconds=3600LL*123+sequence*5;}
  if(alarms){s.alarm={true,0,"","mock: no alarms"};s.activeAlarms=0;s.alarmHistory={true,0,"","mock: empty"};s.alarmHistoryJson="[]";}
  if(extra){s.state={true,0,"","SIMULATED"};Json st;st.flag("known",true);st.flag("servo_on",false);st.flag("running",false);st.flag("alarm",false);st.flag("hold",true);st.str("source","mock");s.stateJson=st.dump();
   s.job={true,0,"","SIMULATED"};s.jobName="MOCK_JOB";Json job;job.str("name","MOCK_JOB");job.num("step",sequence%50);job.str("source","mock");s.jobJson=job.dump();
   s.io={true,0,"","SIMULATED"};Json io;for(int a:d.io)io.num(std::to_string(a),int((sequence+a)%256));s.ioJson=io.dump();}
  return s;}
};

#if defined(RMS_WITH_YMCONNECT) && RMS_WITH_YMCONNECT
// Real SDK known-compatible core. Additional reads are kept isolated below.
class YmReader:public Reader{
 Device d;MotomanController* controller=nullptr;
 public:explicit YmReader(Device dev):d(std::move(dev)){}~YmReader(){disconnect();}
 bool connected()const override{return controller!=nullptr;}
 bool connect()override{disconnect();StatusInfo st{};controller=YMConnect::OpenConnection(d.ip.c_str(),st);if(!controller||st.StatusCode!=0){log("[YMConnect "+d.ip+"] OPEN FAILED code="+std::to_string(st.StatusCode));disconnect();return false;}log("[YMConnect] Connected "+d.ip);return true;}
 void disconnect()override{if(controller){YMConnect::CloseConnection(controller);controller=nullptr;}}
 RobotSample collect(long long,bool extra,bool op,bool alarms)override{
  if(!controller)throw std::runtime_error("Not connected to YRC1000");
  RobotSample out;
  PositionData p{};auto ps=controller->ControlGroup->ReadPositionData(ControlGroupId::R1,CoordinateType::Pulse,0,0,p);
  out.pos={ps.StatusCode==0,int(ps.StatusCode),ps.StatusCode==0?"":"YMConnect position read failed",""};
  if(out.pos.ok)for(int i=0;i<6;i++)out.pulses[i]=static_cast<double>(p.axisData[i]);
  TorqueData t{};auto ts=controller->ControlGroup->ReadTorqueData(ControlGroupId::R1,t);
  out.torque={ts.StatusCode==0,int(ts.StatusCode),ts.StatusCode==0?"":"YMConnect torque read failed",""};
  if(out.torque.ok)for(int i=0;i<6;i++)out.torques[i]=static_cast<double>(t.axisData[i]);
  if(op){
   TimeData td{};auto res=controller->Status->ReadOperatingTimes(ControlGroupId::R1,TimeType::ServoPowerOnTimeRobot,td);
   out.operating={res.StatusCode==0,int(res.StatusCode),res.StatusCode==0?"":"Operating time read failed",res.StatusCode==0?streamIfPossible(td):""};
   // IMPORTANT: time units and TimeData fields require verification against OEM header/pendant.
   // Until verified value_seconds remains null; no fake grease schedule.
  }
  if(alarms){
   ActiveAlarms a{};auto res=controller->Faults->GetActiveAlarms(a);
   out.alarm={res.StatusCode==0,int(res.StatusCode),res.StatusCode==0?"":"Active alarm read failed",res.StatusCode==0?streamIfPossible(a):""};
   // Do not infer number of alarms from a human-formatted string.
   auto hist=tryHistory(controller->Faults);
   out.alarmHistory=hist.first;
   if(out.alarmHistory.ok){Json evidence;evidence.str("sdk_history_text",hist.second);out.alarmHistoryJson=evidence.dump();}
  }
  if(extra){
   auto state=tryControllerState(controller->Status);
   out.state=state.first;
   if(out.state.ok){Json obj;obj.str("sdk_state_text",state.second);obj.flag("structured_fields_verified",false);out.stateJson=obj.dump();}
   auto job=tryJob(controller->Job);
   out.job=job.first;
   if(out.job.ok){Json obj;obj.str("sdk_job_text",job.second);obj.flag("job_name_verified",false);out.jobJson=obj.dump();}
   Json ioValues;bool allIORead=true;
   if(d.io.empty()){out.io={false,-1,"No IO addresses configured", ""};}
   else {
    for(int address:d.io){
     IOByteData value{};
     auto status=controller->Io->ReadByte(address,value);
     Json cell;bool ok=status.StatusCode==0;cell.flag("read_ok",ok);
     cell.num("status_code",int(status.StatusCode));
     if(ok)cell.str("sdk_raw_text",streamIfPossible(value));else {cell.null("sdk_raw_text");allIORead=false;}
     ioValues.raw(std::to_string(address),cell.dump());
    }
    out.io={allIORead,allIORead?0:-1,allIORead?"":"Some IO address reads failed",ioValues.dump()};
    out.ioJson=ioValues.dump();
   }
  }
  return out;
 }
};
#endif
static std::string availability(const Reading&r){Json j;j.flag("read_ok",r.ok);j.num("status_code",r.code);j.str("message",r.error);if(r.ok)j.str("raw",r.raw);else j.null("raw");return j.dump();}
static std::string metadata(const RobotSample&s,bool mock,const Device&d){Json j;j.flag("simulated",mock);j.str("torque_unit","sdk_raw");j.str("position_unit","pulses");j.str("group",d.group);j.raw("controller_state",s.stateJson);j.raw("job",s.jobJson);j.raw("io_values",s.ioJson);j.raw("alarm_history",s.alarmHistoryJson);j.raw("state_read",availability(s.state));j.raw("job_read",availability(s.job));j.raw("io_read",availability(s.io));j.raw("alarm_history_read",availability(s.alarmHistory));return j.dump();}
static std::string telemetryJson(const Device&d,const std::string&session,long long seq,const std::string&at,const RobotSample&s,bool mock){Json j;j.num("robot_id",d.id);j.str("collector_session_id",session);j.num("sequence",seq);j.str("sampled_at",at);j.flag("position_ok",s.pos.ok);j.num("position_status_code",s.pos.code);j.str("position_status_message",s.pos.error);if(s.pos.ok){j.str("position_sampled_at",at);j.raw("position_pulses",axesJson(s.pulses));}else{j.null("position_sampled_at");j.null("position_pulses");}j.flag("torque_ok",s.torque.ok);j.num("torque_status_code",s.torque.code);j.str("torque_status_message",s.torque.error);if(s.torque.ok){j.str("torque_sampled_at",at);j.raw("torque_axes",axesJson(s.torques));}else{j.null("torque_sampled_at");j.null("torque_axes");}j.str("torque_unit","sdk_raw");j.raw("raw_payload",metadata(s,mock,d));return j.dump();}
static std::string opJson(const Device&d,const std::string&at,const RobotSample&s,bool mock){Json j;j.num("robot_id",d.id);j.str("time_type","ServoPowerOnTimeRobot");j.str("sampled_at",at);j.flag("read_ok",s.operating.ok);j.num("status_code",s.operating.code);j.str("status_message",s.operating.error);if(s.operating.ok)j.str("value_raw",s.operating.raw);else j.null("value_raw");if(s.operating.ok&&s.operatingSeconds)j.num("value_seconds",static_cast<long long>(*s.operatingSeconds));else j.null("value_seconds");Json raw;raw.flag("simulated",mock);raw.str("note","value_seconds null until TimeData units verified; no implicit conversion");j.raw("raw_payload",raw.dump());return j.dump();}
static std::string alarmJson(const Device&d,const std::string&at,const RobotSample&s,bool mock){Json j;j.num("robot_id",d.id);j.str("sampled_at",at);j.flag("read_ok",s.alarm.ok);j.num("status_code",s.alarm.code);j.str("status_message",s.alarm.error);if(s.alarm.ok)j.str("sdk_text",s.alarm.raw);else j.null("sdk_text");if(s.alarm.ok&&s.activeAlarms)j.num("active_alarm_count",*s.activeAlarms);else j.null("active_alarm_count");j.null("parsed_alarms");(void)mock;return j.dump();}
static std::string connectionJson(const Device&d,const std::string&at,const std::string&state,bool mock){Json j;j.num("robot_id",d.id);j.str("state",state);j.str("occurred_at",at);j.str("ip_address",d.ip);j.str("message",mock?"MOCK TEST — NOT ACTUAL ROBOT":"YMConnect collector status");Json r;r.flag("simulated",mock);r.str("control_group",d.group);j.raw("raw_payload",r.dump());return j.dump();}
static void waitUntil(Clock::time_point target){while(running&&Clock::now()<target)std::this_thread::sleep_for(std::min(std::chrono::milliseconds(150),std::chrono::duration_cast<std::chrono::milliseconds>(target-Clock::now())));}
static void loopOne(Config cfg,Device dev,bool mock,bool dry,bool once,const std::string&token){
 Dispatcher tx(cfg,dev,dry,token);std::unique_ptr<Reader>reader;
 if(mock)reader=std::make_unique<MockReader>(dev);
 else{
#if defined(RMS_WITH_YMCONNECT) && RMS_WITH_YMCONNECT
 reader=std::make_unique<YmReader>(dev);
#else
 throw std::runtime_error("Real collector needs -DRMS_ENABLE_YMCONNECT=ON and matching SDK");
#endif
 }
 std::string session=uuid();long long sequence=0;bool announced=false;auto nextOp=Clock::now(),nextAlarm=nextOp,nextExtra=nextOp;
 log("[RMS] robot_id="+std::to_string(dev.id)+" ip="+dev.ip+(mock?" MOCK":" REAL")+" started");
 while(running){auto start=Clock::now();if(!reader->connected()&&!reader->connect()){if(announced){tx.post("/api/v1/collector/connections",connectionJson(dev,timestamp(),"offline",mock));announced=false;}tx.flush();if(once)break;waitUntil(start+std::chrono::milliseconds(cfg.telemetryMs));continue;}
  auto now=Clock::now();bool doOp=now>=nextOp,doAlarm=now>=nextAlarm,doExtras=now>=nextExtra;
  RobotSample r=reader->collect(sequence,doExtras,doOp,doAlarm);
  bool good=r.pos.ok||r.torque.ok;std::string at=timestamp();
  if(good&&!announced){tx.post("/api/v1/collector/connections",connectionJson(dev,at,"connected",mock));announced=true;}
  if(!good&&announced){tx.post("/api/v1/collector/connections",connectionJson(dev,at,"offline",mock));announced=false;reader->disconnect();}
  if(good){tx.post("/api/v1/collector/telemetry",telemetryJson(dev,session,sequence,at,r,mock));}
  if(doOp){tx.post("/api/v1/collector/operating-times",opJson(dev,timestamp(),r,mock));nextOp=now+std::chrono::seconds(cfg.operatingS);}
  if(doAlarm){tx.post("/api/v1/collector/alarms/snapshots",alarmJson(dev,timestamp(),r,mock));nextAlarm=now+std::chrono::seconds(cfg.alarmS);}
  if(doExtras){nextExtra=now+std::chrono::seconds(cfg.extrasS);if(!r.state.ok||!r.job.ok||!r.io.ok)log("[SDK "+std::to_string(dev.id)+"] extras: "+r.state.error+"; "+r.job.error+"; "+r.io.error);}
  tx.flush();if(once)break;++sequence;waitUntil(start+std::chrono::milliseconds(cfg.telemetryMs));
 }
 if(announced&&!once)tx.post("/api/v1/collector/connections",connectionJson(dev,timestamp(),"stopped",mock));
 reader->disconnect();
}
int main(int argc,char**argv){
 try{std::string path="config/collector.conf";bool mock=false,real=false,once=false,dry=false,probe=false,allowMockPost=false;int selected=0;
 for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--config"&&i+1<argc)path=argv[++i];else if(a=="--robot-id"&&i+1<argc)selected=integer("robot-id",argv[++i],1,1000000);else if(a=="--mock")mock=true;else if(a=="--real")real=true;else if(a=="--once")once=true;else if(a=="--dry-run")dry=true;else if(a=="--probe")probe=true;else if(a=="--allow-mock-post")allowMockPost=true;else if(a=="--help"){std::cout<<"rms_collector --config config/collector.conf --mock|--real [--once] [--dry-run] [--robot-id N] [--probe] [--allow-mock-post]\n";return 0;}else throw std::runtime_error("Unknown argument "+a);}
 if(mock&&real)throw std::runtime_error("Cannot use --mock and --real together");
 if(mock&&!dry&&!allowMockPost)throw std::runtime_error("MOCK POST blocked. Use --dry-run or explicitly --allow-mock-post with a DEMO robot ID only");
 if(!mock&&!real&&!probe)throw std::runtime_error("Choose --mock, --real, or --probe");
 Config cfg=configFrom(path);
 std::signal(SIGINT,onSignal);std::signal(SIGTERM,onSignal);
#if !defined(_WIN32)
 umask(0077);
#endif
 if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("libcurl startup failed");
 struct C{~C(){curl_global_cleanup();}} clean;
 std::string token=dry||probe?"":readToken(cfg.token);
 if(probe){Http h(cfg,"");auto r=h.run("/up",{},false);log("[PROBE] GET "+cfg.base+"/up -> "+r.summary());return r.error==CURLE_OK&&r.status>=200&&r.status<400?0:1;}
 std::vector<std::thread>pool;std::vector<std::exception_ptr>errors;std::mutex errorsMutex;
 for(auto dev:cfg.devices){if(selected&&dev.id!=selected)continue;pool.emplace_back([&,dev]{try{loopOne(cfg,dev,mock,dry,once,token);}catch(...){std::lock_guard<std::mutex>l(errorsMutex);errors.push_back(std::current_exception());running=false;}});}
 if(pool.empty())throw std::runtime_error("Selected robot_id not in config");
 for(auto&t:pool)t.join();
 if(!errors.empty())std::rethrow_exception(errors.front());
 log("[RMS] Done");return 0;
 }catch(const std::exception&e){std::cerr<<"[ERROR] "<<e.what()<<'\n';return 1;}
}
