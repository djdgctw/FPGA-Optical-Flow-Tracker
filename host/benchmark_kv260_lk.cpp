// KV260 coefficient-domain LK: independent serial stage clocks and bounded
// pipelines. Prepared modes preload IMAGES AND POINTS, never cropped patches.
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "kv260_patch.hpp"
#include "kv260_flow.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
#ifndef LK_BENCH_LANES
#define LK_BENCH_LANES 3
#endif
constexpr int N = 150, L = LK_BENCH_LANES, P = 64, S = (N + L - 1) / L;
static_assert(L >= 1 && L <= 3, "supported physical lane counts: 1, 2, 3");
const char* const KERNEL_NAME = std::getenv("LK_BENCH_KERNEL") ? std::getenv("LK_BENCH_KERNEL") :
                              L == 1 ? "lk_coeff_single" :
                              L == 2 ? "lk_coeff_dual" : "lk_coeff_triple";
constexpr size_t PATCH = P * P, INPUT = S * 2 * PATCH, OUTPUT = S * sizeof(int64_t);
double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b-a).count();
}
bool setting(const char* name) {
  auto p = std::getenv(name);
  return p && std::string(p) == "1";
}
std::vector<int> cpu_plan() {
  auto p = std::getenv("LK_PIPELINE_CPU_LIST");
  std::vector<int> result;
  if (!p) return result;
  cpu_set_t allowed;
  if (sched_getaffinity(0, sizeof(allowed), &allowed)) throw std::runtime_error("get affinity");
  std::istringstream in(p);
  std::string item;
  while (std::getline(in, item, ',')) {
    size_t end;
    int cpu = std::stoi(item, &end);
    if (end != item.size() || cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed) ||
        std::find(result.begin(), result.end(), cpu) != result.end())
      throw std::runtime_error("invalid LK_PIPELINE_CPU_LIST");
    result.push_back(cpu);
  }
  return result;
}
void pin(int cpu) {
  cpu_set_t mask;
  CPU_ZERO(&mask); CPU_SET(cpu, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask)) throw std::runtime_error("set affinity");
}
cv::Mat decode(const fs::path& file) {
  auto a = cv::imread(file.string(), cv::IMREAD_GRAYSCALE);
  if (a.empty()) throw std::runtime_error("cannot decode " + file.string());
  return a;
}
std::vector<cv::Point2f> detect(const cv::Mat& a) {
  std::vector<cv::Point2f> all, selected;
  cv::goodFeaturesToTrack(a, all, 450, .01, 10.0);
  selected.reserve(N);
  for (auto p : all) {
    if (p.x < 32 || p.y < 32 || p.x >= a.cols-32 || p.y >= a.rows-32) continue;
    selected.push_back(p);
    if (selected.size() == N) break;
  }
  return selected;
}
// Cache only adjacent-frame reuse, bounded by in-flight slots. All decodes in
// full modes occur INSIDE the outer wall timer, including pipeline fill/drain.
class Cache {
  struct Entry {
    std::promise<cv::Mat> promise;
    std::shared_future<cv::Mat> future = promise.get_future().share();
    int users = 0;
  };
  const std::vector<fs::path>& files;
  std::mutex mutex;
  std::unordered_map<size_t, std::shared_ptr<Entry>> entries;
 public:
  std::atomic<int> decodes{0};
  explicit Cache(const std::vector<fs::path>& f) : files(f) {}
  cv::Mat get(size_t index) {
    std::shared_ptr<Entry> entry;
    bool owner;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = entries.find(index);
      owner = it == entries.end();
      if (owner) entries[index] = entry = std::make_shared<Entry>();
      else entry = it->second;
      if (++entry->users == (index == 0 || index+1 == files.size() ? 1 : 2)) entries.erase(index);
    }
    if (owner) {
      try { auto a = decode(files[index]); ++decodes; entry->promise.set_value(a); }
      catch (...) { entry->promise.set_exception(std::current_exception()); }
    }
    return entry->future.get();
  }
};
struct Sample {
  int points = 0;
  double read = 0, detect = 0, crop = 0, pack = 0, submit = 0, execute = 0;
  double result = 0, tracking = 0, other = 0, latency = 0, launch_to_complete = 0;
};
class Source {
 public:
  std::vector<fs::path> files;
  std::vector<cv::Mat> images;
  std::vector<std::vector<cv::Point2f>> points;
  std::unique_ptr<Cache> cache;
  bool prepared, reuse, images_only;
  bool imported_points = false;
  double preload_ms = 0;
  size_t resident_bytes = 0;
  Source(const fs::path& dir, int pairs, bool prep, bool re, bool image_ready=false)
      : prepared(prep), reuse(re), images_only(image_ready) {
    if (images_only && std::getenv("LK_POINTS_PREFIX"))
      throw std::runtime_error("image-ready detection must not import prepared points");
    for (auto& f : fs::directory_iterator(dir))
      if (f.is_regular_file() && f.path().extension() == ".png") files.push_back(f.path());
    std::sort(files.begin(), files.end());
    if (files.size() < size_t(pairs+1)) throw std::runtime_error("not enough PNG files");
    files.resize(pairs+1);
    cache = std::make_unique<Cache>(files);
    if (prepared || images_only) {
      const auto begin = Clock::now();
      size_t limit = 512ULL << 20;
      if (auto env = std::getenv("LK_RESIDENT_LIMIT_MIB")) limit = std::stoull(env) << 20;
      for (auto& f : files) {
        auto a = decode(f);
        resident_bytes += a.total()*a.elemSize();
        if (resident_bytes > limit) throw std::runtime_error("prepared images exceed resident limit");
        if (!images.empty() && a.size() != images[0].size()) throw std::runtime_error("image sizes differ");
        images.push_back(a);
      }
      if (images_only) {
        // Deliberately leave feature detection inside each timed task.
      } else if (auto prefix = std::getenv("LK_POINTS_PREFIX")) {
        std::ifstream counts(std::string(prefix)+".counts.bin",std::ios::binary);
        std::ifstream coordinates(std::string(prefix)+".points.bin",std::ios::binary);
        std::ifstream names(std::string(prefix)+".files.txt");
        if(!counts||!coordinates||!names) throw std::runtime_error("invalid LK_POINTS_PREFIX artifacts");
        for(auto& file:files) {
          std::string name; std::getline(names,name);
          if(name!=file.filename().string()) throw std::runtime_error("prepared feature file order mismatch");
        }
        for(int i=0;i<pairs;++i) {
          int32_t count=-1; counts.read(reinterpret_cast<char*>(&count),sizeof(count));
          if(!counts||count<0||count>N) throw std::runtime_error("invalid prepared feature count");
          std::vector<cv::Point2f> selected(count);
          for(auto& p:selected) {
            coordinates.read(reinterpret_cast<char*>(&p.x),sizeof(float));
            coordinates.read(reinterpret_cast<char*>(&p.y),sizeof(float));
            if(!coordinates||!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.y<0||
               p.x>=images[i].cols||p.y>=images[i].rows) throw std::runtime_error("invalid prepared coordinates");
          }
          points.push_back(std::move(selected));
        }
        imported_points=true;
      } else for (int i=0; i<pairs; ++i) points.push_back(detect(images[i]));
      preload_ms = ms(begin, Clock::now());
    }
  }
  void get(size_t frame, cv::Mat& a, cv::Mat& b, std::vector<cv::Point2f>& selected, Sample& s) {
    auto t = Clock::now();
    if (images_only) {
      a = images[frame]; b = images[frame+1];
      auto detection_begin=Clock::now();
      selected=detect(a);
      s.detect=ms(detection_begin,Clock::now());
      return;
    }
    if (prepared) {
      a = images[frame]; b = images[frame+1]; selected = points[frame];
      return;
    }
    a = reuse ? cache->get(frame) : decode(files[frame]);
    b = reuse ? cache->get(frame+1) : decode(files[frame+1]);
    if (a.size() != b.size()) throw std::runtime_error("image sizes differ");
    auto end = Clock::now();
    s.read = ms(t, end);
    selected = detect(a);
    s.detect = ms(end, Clock::now());
  }
};
struct Slot {
  std::array<xrt::bo,L> input, u, v;
  std::array<uint8_t*,L> mapped{};
  std::array<int64_t*,L> mapped_u{}, mapped_v{};
  std::vector<uint8_t> scratch;
  std::vector<cv::Point2f> selected;
  xrt::run run;
  size_t frame = 0;
  Sample sample;
  Clock::time_point began, tracking_begin, launched;
  bool staged, worker_sync, halfpixel;
  Slot(xrt::device& dev, xrt::kernel& kernel, bool st, bool ws, bool cacheable)
      : scratch(st ? N*2*PATCH : 0), run(kernel), staged(st), worker_sync(ws), halfpixel(setting("LK_HALFPIX_NEON")) {
    for (int l=0; l<L; ++l) {
      input[l] = xrt::bo(dev, INPUT, cacheable ? xrt::bo::flags::cacheable : xrt::bo::flags::normal,
                         kernel.group_id(3*l));
      u[l] = xrt::bo(dev, OUTPUT, kernel.group_id(3*l+1));
      v[l] = xrt::bo(dev, OUTPUT, kernel.group_id(3*l+2));
      mapped[l] = input[l].map<uint8_t*>();
      mapped_u[l] = u[l].map<int64_t*>(); mapped_v[l] = v[l].map<int64_t*>();
      run.set_arg(3*l, input[l]); run.set_arg(3*l+1,u[l]); run.set_arg(3*l+2,v[l]);
    }
    selected.reserve(N);
    run.set_arg(3*L+1,7);
    run.set_arg(3*L+2, int64_t(std::llround(.01 * (1ULL<<32))));
  }
  void prepare(Source& source, size_t index) {
    frame=index; sample=Sample{}; began=Clock::now();
    cv::Mat a,b;
    source.get(index,a,b,selected,sample);
    sample.points = int(selected.size());
    tracking_begin=Clock::now();
    auto t=Clock::now();
    if (!staged && sample.points < N) for(auto ptr:mapped) std::memset(ptr,0,INPUT);
    for(int i=0;i<sample.points;++i) {
      auto ptr = staged ? scratch.data()+i*2*PATCH : mapped[i%L]+(i/L)*2*PATCH;
      cv::Mat pa(P,P,CV_8UC1,ptr), pb(P,P,CV_8UC1,ptr+PATCH);
      if(halfpixel) {halfpixel_patch(a,selected[i],pa); halfpixel_patch(b,selected[i],pb);}
      else {cv::getRectSubPix(a,cv::Size(P,P),selected[i],pa); cv::getRectSubPix(b,cv::Size(P,P),selected[i],pb);}
    }
    auto end=Clock::now(); sample.crop=ms(t,end); t=Clock::now();
    if(staged) {
      if(sample.points<N) for(auto ptr:mapped) std::memset(ptr,0,INPUT);
      for(int i=0;i<sample.points;++i)
        std::memcpy(mapped[i%L]+(i/L)*2*PATCH,scratch.data()+i*2*PATCH,2*PATCH);
    }
    sample.pack=ms(t,Clock::now());
    if(worker_sync) upload();
  }
  void upload() {
    auto t=Clock::now();
    const size_t bytes = ((sample.points+L-1)/L)*2*PATCH;
    if(bytes) for(auto& bo:input) bo.sync(XCL_BO_SYNC_BO_TO_DEVICE,bytes,0);
    sample.submit += ms(t,Clock::now());
  }
  void start() {
    auto t=Clock::now();
    run.set_arg(3*L,sample.points);
    // Include all necessary scalar setup even though iteration/epsilon are invariant.
    run.set_arg(3*L+1,7); run.set_arg(3*L+2,int64_t(std::llround(.01*(1ULL<<32))));
    launched=Clock::now(); run.start();
    sample.submit += ms(t,Clock::now());
  }
  void wait() {
    auto t=Clock::now();
    auto state=ERT_CMD_STATE_NEW;
    if(setting("LK_WAIT_POLL")) {
      do {
        state=run.state();
        if(state>=ERT_CMD_STATE_COMPLETED) break;
        if(ms(t,Clock::now())>60000) {
          // Keep the BO alive until XRT has drained the in-flight command.
          run.wait();
          throw std::runtime_error("kernel polling timeout");
        }
      } while(true);
    } else state=run.wait();
    if(state!=ERT_CMD_STATE_COMPLETED) throw std::runtime_error("kernel failed");
    auto end=Clock::now();
    sample.execute=ms(t,end); sample.launch_to_complete=ms(launched,end);
  }
  void download(int64_t* raw, float* coords, float* inputs) {
    auto t=Clock::now();
    for(int l=0;l<L;++l) {
      int count=(sample.points+L-1-l)/L;
      if(count<=0) continue;
      u[l].sync(XCL_BO_SYNC_BO_FROM_DEVICE,count*sizeof(int64_t),0);
      v[l].sync(XCL_BO_SYNC_BO_FROM_DEVICE,count*sizeof(int64_t),0);
      for(int j=0;j<count;++j) {
        int i=l+j*L;
        raw[2*i]=mapped_u[l][j]; raw[2*i+1]=mapped_v[l][j];
        coords[2*i]=selected[i].x+kv260_flow_pixels(uint64_t(raw[2*i]));
        coords[2*i+1]=selected[i].y+kv260_flow_pixels(uint64_t(raw[2*i+1]));
      }
    }
    auto end=Clock::now();
    sample.result=ms(t,end); sample.tracking=ms(tracking_begin,end);
    sample.latency=ms(began,end);
    sample.other=sample.tracking-sample.crop-sample.pack-sample.submit-sample.execute-sample.result;
    // Diagnostic input copy is outside the per-call tracking timer. The full
    // throughput wall timer conservatively includes this small bookkeeping.
    for(int i=0;i<sample.points;++i) { inputs[2*i]=selected[i].x; inputs[2*i+1]=selected[i].y; }
  }
};
struct Result {
  std::vector<Sample> samples;
  std::vector<int64_t> raw;
  std::vector<float> coords, inputs;
  double wall=0;
  explicit Result(int pairs):samples(pairs),raw(pairs*N*2),coords(pairs*N*2),inputs(pairs*N*2){}
  void consume(Slot& slot) {
    slot.download(raw.data()+slot.frame*N*2,coords.data()+slot.frame*N*2,inputs.data()+slot.frame*N*2);
    samples[slot.frame]=slot.sample;
  }
};
struct Shared {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Slot*> free;
  std::vector<Slot*> ready;
  size_t next=0;
  bool stopped=false;
  std::exception_ptr error;
  explicit Shared(size_t pairs):ready(pairs,nullptr){}
  void fail(std::exception_ptr e) {
    std::lock_guard<std::mutex> lock(mutex);
    if(!error) error=e;
    stopped=true; cv.notify_all();
  }
  Slot* take(size_t i) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock,[&]{return stopped || ready[i];});
    if(error) std::rethrow_exception(error);
    if(stopped) throw std::runtime_error("cancelled");
    return ready[i];
  }
};
void pipeline(std::vector<std::unique_ptr<Slot>>& slots,Source& source,Result& result,
              int workers,const std::vector<int>& cpus) {
  Shared q(result.samples.size());
  for(auto& s:slots) q.free.push_back(s.get());
  std::vector<std::thread> threads;
  Slot* active=nullptr;
  auto begin=Clock::now();
  try {
    for(int w=0;w<workers;++w) threads.emplace_back([&,w]{
      try {
        if(!cpus.empty()) pin(cpus[w+1]);
        while(true) {
          Slot* s; size_t i;
          {
            std::unique_lock<std::mutex> lock(q.mutex);
            q.cv.wait(lock,[&]{return q.stopped || q.next>=q.ready.size() || !q.free.empty();});
            if(q.stopped || q.next>=q.ready.size()) return;
            s=q.free.front(); q.free.pop_front(); i=q.next++;
          }
          s->prepare(source,i);
          { std::lock_guard<std::mutex> lock(q.mutex); q.ready[i]=s; q.cv.notify_all(); }
        }
      } catch(...) {q.fail(std::current_exception());}
    });
    Slot* current=q.take(0);
    if(!current->worker_sync) current->upload();
    current->start(); active=current;
    for(size_t i=0;i<q.ready.size();++i) {
      Slot* next=nullptr;
      if(i+1<q.ready.size()) {
        next=q.take(i+1);
        if(!next->worker_sync) next->upload();
      }
      current->wait(); active=nullptr;
      if(next) {next->start(); active=next;}
      result.consume(*current);
      {std::lock_guard<std::mutex> lock(q.mutex); q.free.push_back(current); q.cv.notify_all();}
      current=next;
    }
  } catch(...) {q.fail(std::current_exception());}
  {std::lock_guard<std::mutex> lock(q.mutex); q.stopped=true; q.cv.notify_all();}
  for(auto& t:threads) t.join();
  if(active) {try{active->wait();}catch(...){q.fail(std::current_exception());}}
  result.wall=ms(begin,Clock::now());
  if(q.error) std::rethrow_exception(q.error);
}
int main(int argc,char** argv) {
  try {
    if(argc!=10) throw std::runtime_error(
      "usage: benchmark_kv260_lk XCLBIN IMAGE_DIR PAIRS stages|prepared-serial|prepared-pipeline|detect-serial|detect-pipeline|full-serial|full-pipeline WORKERS SLOTS staged|direct reuse|independent OUTPUT_PREFIX");
    int pairs=std::stoi(argv[3]),workers=std::stoi(argv[5]),nslots=std::stoi(argv[6]);
    std::string mode=argv[4], packing=argv[7], decode_mode=argv[8], prefix=argv[9];
    bool pipe=mode=="prepared-pipeline"||mode=="full-pipeline"||mode=="detect-pipeline";
    bool prepared=mode=="stages"||mode=="prepared-serial"||mode=="prepared-pipeline";
    bool image_ready=mode=="detect-serial"||mode=="detect-pipeline";
    if((!prepared && !image_ready && mode!="full-serial" && mode!="full-pipeline") || pairs<1 || workers<1 || workers>8 ||
       nslots<2 || nslots>16 || (packing!="staged"&&packing!="direct") ||
       (decode_mode!="reuse"&&decode_mode!="independent")) throw std::runtime_error("invalid arguments");
    auto cpus=cpu_plan();
    if(pipe&&!cpus.empty()&&cpus.size()<size_t(workers+1)) throw std::runtime_error("too few affinity CPUs");
    for(auto ext:{".json",".csv",".flows.bin",".coords.bin",".points.bin",".counts.bin",".files.txt"})
      if(fs::exists(prefix+ext)) throw std::runtime_error("refusing overwrite "+prefix+ext);
    cv::setNumThreads(1); cv::setUseOptimized(true);
    Source source(argv[2],pairs,prepared,decode_mode=="reuse",image_ready);
    xrt::device dev(0);
    auto uuid=dev.load_xclbin(argv[1]);
    xrt::kernel kernel(dev,uuid,KERNEL_NAME);
    bool ws=pipe&&setting("LK_WORKER_SYNC"), cacheable=setting("LK_BO_CACHEABLE");
    std::vector<std::unique_ptr<Slot>> slots;
    for(int i=0;i<(pipe?nslots:1);++i)
      slots.push_back(std::make_unique<Slot>(dev,kernel,packing=="staged",ws,cacheable));
    Result result(pairs);
    if(!cpus.empty()) pin(cpus[0]);
    if(pipe) pipeline(slots,source,result,workers,cpus);
    else {
      auto begin=Clock::now();
      for(int i=0;i<pairs;++i) {
        auto& s=*slots[0]; s.prepare(source,i); s.upload(); s.start(); s.wait(); result.consume(s);
      }
      result.wall=ms(begin,Clock::now());
    }
    auto parent=fs::path(prefix).parent_path(); if(!parent.empty()) fs::create_directories(parent);
    std::ofstream raw(prefix+".flows.bin",std::ios::binary),coords(prefix+".coords.bin",std::ios::binary),
      inputs(prefix+".points.bin",std::ios::binary),counts(prefix+".counts.bin",std::ios::binary),
      csv(prefix+".csv"),json(prefix+".json"),names(prefix+".files.txt");
    for(auto& f:source.files) names<<f.filename().string()<<'\n';
    csv<<"frame,points,read_ms,detect_ms,crop_ms,pack_ms,submit_ms,execute_host_wait_ms,return_ms,tracking_outer_ms,other_ms,pair_latency_ms,kernel_start_to_observed_complete_ms,frontend_outer_ms,frontend_other_ms\n"<<std::setprecision(12);
    int64_t features=0; double checksum=0;
    for(int i=0;i<pairs;++i) {
      auto& s=result.samples[i]; int32_t count=s.points; features+=count;
      counts.write(reinterpret_cast<char*>(&count),sizeof(count));
      raw.write(reinterpret_cast<char*>(result.raw.data()+i*N*2),count*2*sizeof(int64_t));
      coords.write(reinterpret_cast<char*>(result.coords.data()+i*N*2),count*2*sizeof(float));
      inputs.write(reinterpret_cast<char*>(result.inputs.data()+i*N*2),count*2*sizeof(float));
      for(int j=0;j<count*2;++j) checksum+=result.coords[i*N*2+j];
      csv<<i<<','<<count<<','<<s.read<<','<<s.detect<<','<<s.crop<<','<<s.pack<<','<<s.submit<<','<<s.execute
         <<','<<s.result<<','<<s.tracking<<','<<s.other<<','<<s.latency<<','<<s.launch_to_complete
         <<','<<s.latency<<','<<s.latency-s.read-s.detect-s.crop-s.pack-s.submit-s.execute-s.result<<'\n';
    }
    auto mean=[&](double Sample::*m){double sum=0;for(auto& s:result.samples)sum+=s.*m;return sum/pairs;};
    json<<std::setprecision(12)<<"{\n\"mode\":\""<<mode<<"\",\"packing\":\""<<packing<<"\",\"decode_mode\":\""<<decode_mode
        <<"\",\"pairs\":"<<pairs<<",\"features\":"<<features<<",\"mean_features\":"<<double(features)/pairs
        <<",\"workers\":"<<(pipe?workers:0)<<",\"slots\":"<<slots.size()<<",\"worker_sync\":"<<(ws?"true":"false")
        <<",\"cacheable_input_bo\":"<<(cacheable?"true":"false")<<",\"halfpixel_neon\":"<<(setting("LK_HALFPIX_NEON")?"true":"false")
        <<",\"wait_poll\":"<<(setting("LK_WAIT_POLL")?"true":"false")
        <<",\"physical_lanes\":"<<L<<",\"points_per_lane_capacity\":"<<S
        <<",\"kernel_name\":\""<<KERNEL_NAME<<"\""
        <<",\"flow_width\":40,\"flow_fractional_bits\":32,\"sign_extension\":true"
        <<",\"prepared_images_and_points\":"<<(prepared?"true":"false")
        <<",\"preloaded_images\":"<<((prepared||image_ready)?"true":"false")
        <<",\"detection_inside\":"<<(!prepared?"true":"false")
        <<",\"imported_prepared_points\":"<<(source.imported_points?"true":"false")
        <<",\"preload_outside_ms\":"<<source.preload_ms<<",\"resident_bytes\":"<<source.resident_bytes
        <<",\"wall_ms\":"<<result.wall<<",\"fps\":"<<pairs*1000./result.wall
        <<",\"features_per_second\":"<<features*1000./result.wall
        <<",\"serial_tracking_fps\":"<<(pipe?0:pairs*1000./(pairs*mean(&Sample::tracking)))
        <<",\"coordinate_checksum\":"<<checksum<<",\"opencv_version\":\""<<CV_VERSION
        <<"\",\"png_decodes_inside\":"<<((prepared||image_ready)?0:(source.reuse?source.cache->decodes.load():pairs*2))
        <<",\"includes_fill_drain\":true,\"execute_is_host_wait_not_pl_busy\":true,\"mean_ms\":{"
        <<"\"read\":"<<mean(&Sample::read)<<",\"detect\":"<<mean(&Sample::detect)<<",\"crop\":"<<mean(&Sample::crop)
        <<",\"pack\":"<<mean(&Sample::pack)<<",\"submit\":"<<mean(&Sample::submit)
        <<",\"execute_host_wait\":"<<mean(&Sample::execute)<<",\"return\":"<<mean(&Sample::result)
        <<",\"tracking_outer\":"<<mean(&Sample::tracking)<<",\"other\":"<<mean(&Sample::other)
        <<",\"pair_latency\":"<<mean(&Sample::latency)<<",\"kernel_start_to_observed_complete\":"<<mean(&Sample::launch_to_complete)
        <<"},\"cpu_affinity\":[";
    for(size_t i=0;i<cpus.size();++i) json<<(i?",":"")<<cpus[i];
    json<<"]}\n";
    if(!raw||!coords||!inputs||!counts||!csv||!json||!names) throw std::runtime_error("output write failed");
    std::cout<<mode<<" "<<packing<<" workers="<<workers<<" slots="<<slots.size()<<" fps="<<pairs*1000./result.wall
             <<" crop_ms="<<mean(&Sample::crop)<<" pack_ms="<<mean(&Sample::pack)<<" submit_ms="<<mean(&Sample::submit)
             <<" wait_ms="<<mean(&Sample::execute)<<" return_ms="<<mean(&Sample::result)<<'\n';
  }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
