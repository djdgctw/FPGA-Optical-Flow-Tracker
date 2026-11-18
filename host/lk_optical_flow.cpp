// KV260 Gaussian-pyramid optical flow, three hardware lanes.
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "kv260_patch.hpp"
#include "kv260_flow.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr int N = 150, L = 3, P = 64, S = (N + L - 1) / L;
constexpr int WORKERS = 3, SLOTS = 8;
constexpr const char* KERNEL_NAME = "lk_coeff_triple";
constexpr size_t PATCH = P * P, INPUT = S * 2 * PATCH, OUTPUT = S * sizeof(int64_t);
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
// Decode images and detect features before starting the tracking pipeline.
struct Source {
  std::vector<cv::Mat> images;
  std::vector<std::vector<cv::Point2f>> points;
  Source(const fs::path& dir, size_t pairs) {
    std::vector<fs::path> files;
    for (auto& f : fs::directory_iterator(dir))
      if (f.is_regular_file() && f.path().extension() == ".png") files.push_back(f.path());
    std::sort(files.begin(), files.end());
    if (files.size() < pairs + 1) throw std::runtime_error("not enough PNG files");
    files.resize(pairs + 1);
    size_t resident_bytes = 0;
    for (auto& file : files) {
      auto a = decode(file);
      resident_bytes += a.total() * a.elemSize();
      if (resident_bytes > (512ULL << 20))
        throw std::runtime_error("images exceed 512 MiB; use fewer frame pairs");
      if (!images.empty() && a.size() != images[0].size())
        throw std::runtime_error("image sizes differ");
      images.push_back(a);
    }
    for (size_t i = 0; i < pairs; ++i) points.push_back(detect(images[i]));
  }
};
struct FrameResult {
  std::array<float, N*2> flow{}, coords{};
};
struct Slot {
  std::array<xrt::bo,L> input, u, v;
  std::array<uint8_t*,L> mapped{};
  std::array<int64_t*,L> mapped_u{}, mapped_v{};
  std::vector<cv::Point2f> selected;
  xrt::run run;
  size_t frame = 0;
  int count = 0;
  Slot(xrt::device& dev, xrt::kernel& kernel) : run(kernel) {
    for (int l=0; l<L; ++l) {
      input[l] = xrt::bo(dev, INPUT, xrt::bo::flags::normal, kernel.group_id(3*l));
      u[l] = xrt::bo(dev, OUTPUT, kernel.group_id(3*l+1));
      v[l] = xrt::bo(dev, OUTPUT, kernel.group_id(3*l+2));
      mapped[l] = input[l].map<uint8_t*>();
      mapped_u[l] = u[l].map<int64_t*>(); mapped_v[l] = v[l].map<int64_t*>();
      run.set_arg(3*l, input[l]); run.set_arg(3*l+1,u[l]); run.set_arg(3*l+2,v[l]);
    }
    selected.reserve(N);
    run.set_arg(3*L+1,7);
    run.set_arg(3*L+2,int64_t(std::llround(.01*(1ULL<<32))));
  }
  void prepare(const Source& source, size_t index) {
    frame = index;
    selected = source.points[index];
    count = int(selected.size());
    if (count < N) for (auto ptr : mapped) std::memset(ptr,0,INPUT);
    for (int i=0; i<count; ++i) {
      auto ptr = mapped[i%L]+(i/L)*2*PATCH;
      cv::Mat pa(P,P,CV_8UC1,ptr), pb(P,P,CV_8UC1,ptr+PATCH);
      halfpixel_patch(source.images[index],selected[i],pa);
      halfpixel_patch(source.images[index+1],selected[i],pb);
    }
  }
  void upload() {
    const size_t bytes = ((count+L-1)/L)*2*PATCH;
    if (bytes) for (auto& bo : input) bo.sync(XCL_BO_SYNC_BO_TO_DEVICE,bytes,0);
  }
  void start() {
    run.set_arg(3*L,count);
    run.set_arg(3*L+1,7);
    run.set_arg(3*L+2,int64_t(std::llround(.01*(1ULL<<32))));
    run.start();
  }
  void wait() {
    if (run.wait() != ERT_CMD_STATE_COMPLETED) throw std::runtime_error("kernel failed");
  }
  void download(FrameResult& result) {
    for (int l=0; l<L; ++l) {
      int lane_count = (count+L-1-l)/L;
      if (lane_count <= 0) continue;
      u[l].sync(XCL_BO_SYNC_BO_FROM_DEVICE,lane_count*sizeof(int64_t),0);
      v[l].sync(XCL_BO_SYNC_BO_FROM_DEVICE,lane_count*sizeof(int64_t),0);
      for (int j=0; j<lane_count; ++j) {
        int i = l+j*L;
        result.flow[2*i] = kv260_flow_pixels(uint64_t(mapped_u[l][j]));
        result.flow[2*i+1] = kv260_flow_pixels(uint64_t(mapped_v[l][j]));
        result.coords[2*i] = selected[i].x+result.flow[2*i];
        result.coords[2*i+1] = selected[i].y+result.flow[2*i+1];
      }
    }
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
double pipeline(std::vector<std::unique_ptr<Slot>>& slots, const Source& source,
                std::vector<FrameResult>& results) {
  Shared q(results.size());
  for(auto& s:slots) q.free.push_back(s.get());
  std::vector<std::thread> threads;
  Slot* active=nullptr;
  auto begin=Clock::now();
  try {
    for(int w=0;w<WORKERS;++w) threads.emplace_back([&]{
      try {
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
    current->upload();
    current->start(); active=current;
    for(size_t i=0;i<q.ready.size();++i) {
      Slot* next=nullptr;
      if(i+1<q.ready.size()) {
        next=q.take(i+1);
        next->upload();
      }
      current->wait(); active=nullptr;
      if(next) {next->start(); active=next;}
      current->download(results[current->frame]);
      {std::lock_guard<std::mutex> lock(q.mutex); q.free.push_back(current); q.cv.notify_all();}
      current=next;
    }
  } catch(...) {q.fail(std::current_exception());}
  {std::lock_guard<std::mutex> lock(q.mutex); q.stopped=true; q.cv.notify_all();}
  for(auto& t:threads) t.join();
  if(active) {try{active->wait();}catch(...){q.fail(std::current_exception());}}
  if(q.error) std::rethrow_exception(q.error);
  return std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
}
int main(int argc, char** argv) {
  try {
    if (argc != 5) throw std::runtime_error(
      "usage: lk_optical_flow XCLBIN IMAGE_DIR PAIRS OUTPUT_CSV");
    std::string pair_arg = argv[3];
    if (pair_arg.empty() || pair_arg.find_first_not_of("0123456789") != std::string::npos)
      throw std::runtime_error("PAIRS must be a positive integer");
    const auto pairs = std::stoull(pair_arg);
    if (pairs == 0 || pairs >= size_t(std::numeric_limits<int>::max()))
      throw std::runtime_error("PAIRS is out of range");
    const fs::path output = argv[4];
    if (fs::exists(output)) throw std::runtime_error("refusing to overwrite " + output.string());
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
    std::ofstream csv(output);
    if (!csv) throw std::runtime_error("cannot open output " + output.string());
    cv::setNumThreads(1);
    cv::setUseOptimized(true);
    Source source(argv[2],pairs);
    xrt::device dev(0);
    auto uuid = dev.load_xclbin(argv[1]);
    xrt::kernel kernel(dev,uuid,KERNEL_NAME);
    std::vector<std::unique_ptr<Slot>> slots;
    for (int i=0; i<SLOTS; ++i) slots.push_back(std::make_unique<Slot>(dev,kernel));
    std::vector<FrameResult> results(pairs);
    const double elapsed_ms = pipeline(slots,source,results);
    csv << "pair_index,feature_index,x,y,dx,dy,next_x,next_y\n"
        << std::setprecision(std::numeric_limits<float>::max_digits10);
    size_t features = 0;
    for (size_t frame=0; frame<pairs; ++frame) {
      const auto& points = source.points[frame];
      const auto& r = results[frame];
      features += points.size();
      for (size_t i=0; i<points.size(); ++i)
        csv << frame << ',' << i << ',' << points[i].x << ',' << points[i].y
            << ',' << r.flow[2*i] << ',' << r.flow[2*i+1]
            << ',' << r.coords[2*i] << ',' << r.coords[2*i+1] << '\n';
    }
    csv.close();
    if (!csv) throw std::runtime_error("output write failed");
    std::cout << "Processed " << pairs << " frame pairs, " << features << " features.\n"
              << "Tracking FPS (excluding image decoding, feature detection, and CSV writing): "
              << pairs*1000.0/elapsed_ms << "\nResults: " << output.string() << '\n';
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << '\n';
    return 1;
  }
}
