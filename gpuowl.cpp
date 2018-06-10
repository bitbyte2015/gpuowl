// gpuOwL, a GPU OpenCL primality tester for Mersenne numbers.
// Copyright (C) 2017 Mihai Preda.

#include "worktodo.h"
#include "args.h"
#include "kernel.h"
#include "timeutil.h"
#include "state.h"
#include "stats.h"
#include "common.h"

#include <cassert>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <cstdlib>

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include <signal.h>

#ifndef M_PIl
#define M_PIl 3.141592653589793238462643383279502884L
#endif

#define TAU (2 * M_PIl)

// The git revision should be passed through -D on the compiler command line (see Makefile).
#ifndef REV
#define REV
#endif

#define VERSION "2.2-" REV
#define PROGRAM "gpuowl"

static volatile int stopRequested = 0;

void (*oldHandler)(int) = 0;

void myHandler(int dummy) { stopRequested = 1; }

const unsigned BUF_CONST = CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR | CL_MEM_HOST_NO_ACCESS;
const unsigned BUF_RW    = CL_MEM_READ_WRITE | CL_MEM_HOST_NO_ACCESS;

// Sets the weighting vectors direct A and inverse iA (as per IBDWT).
template<typename T>
void genWeights(int W, int H, int E, T *aTab, T *iTab) {
  T *pa = aTab;
  T *pi = iTab;

  int N = 2 * W * H;
  int baseBits = E / N;
  auto iN = 1 / (long double) N;

  for (int line = 0; line < H; ++line) {
    for (int col = 0; col < W; ++col) {
      for (int rep = 0; rep < 2; ++rep) {
        int k = (line + col * H) * 2 + rep;
        int bits  = bitlen(N, E, k);
        assert(bits == baseBits || bits == baseBits + 1);
        auto a = exp2l(extra(N, E, k) * iN);
        auto ia = 1 / (4 * N * a);
        *pa++ = (bits == baseBits) ? a  : -a;
        *pi++ = (bits == baseBits) ? ia : -ia;
      }
    }
  }
}

template<typename T> struct Pair { T x, y; };

using double2 = Pair<double>;
using float2  = Pair<float>;
using uint2   = Pair<u32>;
using ulong2  = Pair<u64>;

double2 U2(double a, double b) { return double2{a, b}; }

// Returns the primitive root of unity of order N, to the power k.
template<typename T2> T2 root1(u32 N, u32 k);

template<> double2 root1<double2>(u32 N, u32 k) {
  long double angle = - TAU / N * k;
  return double2{double(cosl(angle)), double(sinl(angle))};
}

template<typename T2>
T2 *trig(T2 *p, int n, int B) {
  for (int i = 0; i < n; ++i) { *p++ = root1<T2>(B, i); }
  return p;
}

// The generated trig table has two regions:
// - a size-H half-circle.
// - a region of granularity TAU / (2 * W * H), used in squaring.
cl_mem genSquareTrig(cl_context context, int W, int H) {
  const int size = H/2 + W;
  auto *tab = new double2[size];
  auto *end = tab;
  end = trig(end, H/2,     H * 2);
  end = trig(end, W,     W * H * 2);
  assert(end - tab == size);
  cl_mem buf = makeBuf(context, BUF_CONST, sizeof(double2) * size, tab);
  delete[] tab;
  return buf;
}

// Trig table used by transpose. Has two regions:
// - a size-2048 "full circle",
// - a region of granularity W*H and size W*H/2048.
cl_mem genTransTrig(cl_context context, int W, int H) {
  const int size = 2048 + W * H / 2048;
  auto *tab = new double2[size];
  auto *end = tab;
  end = trig(end, 2048, 2048);
  end = trig(end, W * H / 2048, W * H);
  assert(end - tab == size);
  cl_mem buf = makeBuf(context, BUF_CONST, sizeof(double2) * size, tab);
  delete[] tab;
  return buf;
}

template<typename T2>
T2 *smallTrigBlock(int W, int H, T2 *p) {
  for (int line = 1; line < H; ++line) {
    for (int col = 0; col < W; ++col) {
      *p++ = root1<T2>(W * H, line * col);
    }
  }
  return p;
}

cl_mem genSmallTrig(cl_context context, int size, int radix) {
  auto *tab = new double2[size]();
  auto *p = tab + radix;
  int w = 0;
  for (w = radix; w < size; w *= radix) { p = smallTrigBlock(w, std::min(radix, size / w), p); }
  // assert(w == size);
  assert(p - tab == size);
  cl_mem buf = makeBuf(context, BUF_CONST, sizeof(double2) * size, tab);
  delete[] tab;
  return buf;
}

template<typename T>
void setupWeights(cl_context context, Buffer &bufA, Buffer &bufI, int W, int H, int E) {
  int N = 2 * W * H;
  T *aTab    = new T[N];
  T *iTab    = new T[N];
  
  genWeights(W, H, E, aTab, iTab);
  bufA.reset(makeBuf(context, BUF_CONST, sizeof(T) * N, aTab));
  bufI.reset(makeBuf(context, BUF_CONST, sizeof(T) * N, iTab));
  
  delete[] aTab;
  delete[] iTab;
}

// Residue from compacted words.
u64 residue(const std::vector<u32> &words) { return (u64(words[1]) << 32) | words[0]; }

u32 mod3(const std::vector<u32> &words) {
  u32 r = 0;
  // uses the fact that 2**32 % 3 == 1.
  for (u32 w : words) { r += w % 3; }
  return r % 3;
}

void doDiv3(int E, std::vector<u32> &words) {
  u32 r = (3 - mod3(words)) % 3;
  assert(0 <= r && r < 3);

  int topBits = E % 32;
  assert(topBits > 0 && topBits < 32);

  {
    u64 w = (u64(r) << topBits) + words.back();
    words.back() = w / 3;
    r = w % 3;
  }

  for (auto it = words.rbegin() + 1, end = words.rend(); it != end; ++it) {
    u64 w = (u64(r) << 32) + *it;
    *it = w / 3;
    r = w % 3;
  }
}

// Note: pass vector by copy is intentional.
void doDiv9(int E, std::vector<u32> &words) {
  doDiv3(E, words);
  doDiv3(E, words);
}

std::vector<std::unique_ptr<FILE>> logFiles;

void initLog() {
  logFiles.push_back(std::unique_ptr<FILE>(stdout));
  if (auto fo = open("gpuowl.log", "a")) {
#if defined(_DEFAULT_SOURCE) || defined(_BSD_SOURCE)
    setlinebuf(fo.get());
#endif
    logFiles.push_back(std::move(fo));
  }
}

void log(const char *fmt, ...) {
  va_list va;
  for (auto &f : logFiles) {
    va_start(va, fmt);
    vfprintf(f.get(), fmt, va);
    va_end(va);
#if !(defined(_DEFAULT_SOURCE) || defined(_BSD_SOURCE))
    fflush(f.get());
#endif
  }
}

string hexStr(u64 res) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%016llx", res);
  return buf;
}

std::string timeStr() {
  time_t t = time(NULL);
  char buf[64];
  // equivalent to: "%F %T"
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&t));
  return buf;
}

std::string timeStr(const std::string &format) {
  time_t t = time(NULL);
  char buf[64];
  strftime(buf, sizeof(buf), format.c_str(), localtime(&t));
  return buf;
}

std::string longTimeStr()  { return timeStr("%Y-%m-%d %H:%M:%S %Z"); }
std::string shortTimeStr() { return timeStr("%Y-%m-%d %H:%M:%S"); }

std::string makeLogStr(int E, int k, u64 res, const StatsInfo &info, const string &cpuName) {
  int end = ((E - 1) / 1000 + 1) * 1000;
  float percent = 100 / float(end);
  
  int etaMins = (end - k) * info.mean * (1 / 60000.f) + .5f;
  int days  = etaMins / (24 * 60);
  int hours = etaMins / 60 % 24;
  int mins  = etaMins % 60;
  
  char buf[128];
  snprintf(buf, sizeof(buf), "%s %s%8d/%d [%5.2f%%], %.2f ms/it [%.2f, %.2f]; ETA %dd %02d:%02d; %s",
           shortTimeStr().c_str(),
           cpuName.empty() ? "" : (cpuName + " ").c_str(),
           k, E, k * percent, info.mean, info.low, info.high, days, hours, mins,
           hexStr(res).c_str());
  return buf;
}

void doLog(int E, int k, long timeCheck, u64 res, bool checkOK, int nErrors, Stats &stats, bool didSave, const string &cpuName) {
  std::string errors = !nErrors ? "" : ("; (" + std::to_string(nErrors) + " errors)");
  log("%s %s (check %.2fs)%s%s\n",
      checkOK ? "OK" : "EE",
      makeLogStr(E, k, res, stats.getStats(), cpuName).c_str(),
      timeCheck * .001f, errors.c_str(),
      didSave ? " (saved)" : "");
  stats.reset();
}

void doSmallLog(int E, int k, u64 res, Stats &stats, const string &cpuName) {
  printf("   %s\n", makeLogStr(E, k, res, stats.getStats(), cpuName).c_str());
  stats.reset();
}

bool writeResult(int E, bool isPrime, u64 res, const std::string &AID, const std::string &user, const std::string &cpu, int nErrors, int fftSize) {
  std::string uid;
  if (!user.empty()) { uid += ", \"user\":\"" + user + '"'; }
  if (!cpu.empty())  { uid += ", \"computer\":\"" + cpu + '"'; }
  std::string aidJson = AID.empty() ? "" : ", \"aid\":\"" + AID + '"';
  std::string errors = ", \"errors\":{\"gerbicz\":" + std::to_string(nErrors) + "}";
    
  char buf[512];
  snprintf(buf, sizeof(buf),
           R"-({"exponent":%d, "worktype":"PRP-3", "status":"%c", "residue-type":1, "fft-length":"%dK", "res64":"%s", "program":{"name":"%s", "version":"%s"}, "timestamp":"%s"%s%s%s})-",
           E, isPrime ? 'P' : 'C', fftSize / 1024, hexStr(res).c_str(), PROGRAM, VERSION, timeStr().c_str(),
           errors.c_str(), uid.c_str(), aidJson.c_str());
  
  log("%s\n", buf);
  if (auto fo = open("results.txt", "a")) {
    fprintf(fo.get(), "%s\n", buf);
    return true;
  } else {
    return false;
  }
}

void logTimeKernels(std::initializer_list<Kernel *> kerns) {
  struct Info {
    std::string name;
    StatsInfo stats;
  };

  double total = 0;
  std::vector<Info> infos;
  for (Kernel *k : kerns) {
    Info info{k->getName(), k->resetStats()};
    infos.push_back(info);
    total += info.stats.sum;
  }

  std::sort(infos.begin(), infos.end(), [](const Info &a, const Info &b) { return a.stats.sum >= b.stats.sum; });

  for (Info info : infos) {
    StatsInfo stats = info.stats;
    float percent = 100 / total * stats.sum;
    if (percent >= .5f) {
      log("%3.1f%% %-10s : %5.0f [%5.0f, %5.0f] us/call   x %5d calls\n",
          percent, info.name.c_str(), stats.mean, stats.low, stats.high, stats.n);
    }
  }
  log("\n");
}

template<typename T, int N> constexpr int size(T (&)[N]) { return N; }

bool isAllZero(const std::vector<u32> &vect) {
  for (const auto x : vect) { if (x) { return false; } }
  return true;
}

class Checkpoint {
private:
  static constexpr const int CHECK_STEP = 200;

  struct HeaderV4 {
    // <exponent> <iteration> <nErrors> <check-step> <checksum>
    static constexpr const char *HEADER = "OWL 4 %d %d %d %d %016llx\n";

    int E, k, nErrors, checkStep;
    u64 checksum;

    bool parse(const char *line) { return sscanf(line, HEADER, &E, &k, &nErrors, &checkStep, &checksum) == 5; }
    bool write(FILE *fo) { return (fprintf(fo, HEADER, E, k, nErrors, checkStep, checksum) > 0); }
  };
  
  struct HeaderV3 {
    // <exponent> <iteration> <nErrors> <check-step>
    static constexpr const char *HEADER = "OWL 3 %d %d %d %d\n";

    int E, k, nErrors, checkStep;

    bool parse(const char *line) { return sscanf(line, HEADER, &E, &k, &nErrors, &checkStep) == 4; }
    bool write(FILE *fo) { return (fprintf(fo, HEADER, E, k, nErrors, checkStep) > 0); }
  };
  
  static bool write(FILE *fo, const vector<u32> &vect) { return fwrite(vect.data(), vect.size() * sizeof(vect[0]), 1, fo); }
  static std::vector<u32> read(FILE *fi, int n) {
    std::vector<u32> vect(n);
    if (fread(vect.data(), n * sizeof(vect[0]), 1, fi)) { return std::move(vect); }
    throw "Read";
  }
  
  static bool write(int E, const string &name, const std::vector<u32> &check, int k, int nErrors, int checkStep) {
    const int nWords = (E - 1) / 32 + 1;
    assert(int(check.size()) == nWords);

    u64 sum = checksum(check);
    HeaderV4 header{E, k, nErrors, checkStep, sum};
    auto fo(open(name, "wb"));
    return fo
      && header.write(fo.get())
      && write(fo.get(), check);
  }

  static std::string fileName(int E) { return std::to_string(E) + ".owl"; }

  static u64 checksum(const std::vector<u32> &data) {
    u32 a = 1;
    u32 b = 0;
    for (u32 x : data) {
      a += x;
      b += a;
    }
    return (u64(a) << 32) | b;
  }
  
public:
  
  static std::vector<u32> load(int E, int *k, int *nErrors, int *checkStep) {
    *k = 0;
    *nErrors = 0;
    *checkStep = CHECK_STEP;    
    const int nWords = (E - 1) / 32 + 1;
    
    auto fi{open(fileName(E), "rb", false)};    
    if (!fi) {
      std::vector<u32> check(nWords);
      check[0] = 1;
      return check;
    }
    
    char line[256];
    if (!fgets(line, sizeof(line), fi.get())) { throw "checkpoint read header"; }
    
    {
      HeaderV4 header;
      if (header.parse(line)) {
        assert(header.E == E);
        *k = header.k;
        *nErrors = header.nErrors;
        *checkStep = header.checkStep;
        std::vector<u32> check = read(fi.get(), nWords);
        assert(header.checksum == checksum(check));
        return check;
      }        
    }

    {
      HeaderV3 header;
      if (header.parse(line)) {
        assert(header.E == E);
        *k = header.k;
        *nErrors = header.nErrors;
        *checkStep = header.checkStep;
        std::vector<u32> data  = read(fi.get(), nWords);
        std::vector<u32> check = read(fi.get(), nWords);
        return check;
        // return State(E, std::move(data), std::move(check));
      }
    }
    throw "Checkpoint load";
  }
  
  static void save(int E, const vector<u32> &check, int k, int nErrors, int checkStep) {
    string saveFile = fileName(E);
    string strE = std::to_string(E);
    string tempFile = strE + "-temp.owl";
    string prevFile = strE + "-prev.owl";
    
    if (write(E, tempFile, check, k, nErrors, checkStep)) {
      remove(prevFile.c_str());
      rename(saveFile.c_str(), prevFile.c_str());
      rename(tempFile.c_str(), saveFile.c_str());
    }
    const int persistStep = 20'000'000;
    if (k && (k % persistStep == 0)) {
      string persistFile = strE + "." + std::to_string(k) + ".owl";
      write(E, persistFile, check, k, nErrors, checkStep);
    }
  }
};

// compute res64 from balanced uncompressed ("raw") words.
// the first 64 words are "before word 0" and used only for carry into word 0.
u64 residueFromRaw(int N, int E, const vector<int> &words) {
  assert(words.size() == 128);
  int carry = 0;
  for (int i = 0; i < 64; ++i) { carry = (words[i] + carry < 0) ? -1 : 0; }
  
  u64 res = 0;
  int k = 0, hasBits = 0;
  for (auto p = words.begin() + 64, end = words.end(); p < end && hasBits < 64; ++p, ++k) {
    int len = bitlen(N, E, k);
    int w = *p + carry;
    carry = (w < 0) ? -1 : 0;
    if (w < 0) { w += (1 << len); }
    assert(w >= 0 && w < (1 << len));
    res |= u64(w) << hasBits;
    hasBits += len;    
  }
  return res;
}

class Gpu {
  int E;
  int W, H;
  int N, hN, nW, nH, bufSize;
  bool useSplitTail;
  
  cl_context context;
  Queue queue;

  Kernel fftP;
  Kernel fftW;
  Kernel fftH;
  Kernel carryA;
  Kernel carryM;
  Kernel carryB;
  Kernel transposeW;
  Kernel transposeH;
  Kernel square;
  Kernel multiply;
  Kernel tailFused;
  Kernel readResidue;
  Kernel doCheck;
  Kernel compare;
  
  Buffer bufData, bufCheck, bufCheck2, bufGoodData, bufGoodCheck;
  Buffer bufTrigW, bufTrigH, bufTransTrig, bufSquareTrig;
  Buffer bufA, bufI;
  Buffer buf1, buf2, buf3;
  Buffer bufCarry;
  Buffer bufSmallOut;
  
public:
  Gpu(int E, int W, int H, cl_program program, cl_context context, cl_queue queue, cl_device_id device, bool timeKernels, bool useSplitTail) :
    E(E),
    W(W),
    H(H),
    N(W * H * 2), hN(N / 2), nW(8), nH(H / 256), bufSize(N * sizeof(double)),
    useSplitTail(useSplitTail),
    context(context),
    queue(queue),

#define LOAD(name, workSize) name(program, queue, device, workSize, #name, timeKernels)
    LOAD(fftP, hN / nW),
    LOAD(fftW, hN / nW),
    LOAD(fftH, hN / nH),    
    LOAD(carryA, hN / 16),
    LOAD(carryM, hN / 16),
    LOAD(carryB, hN / 16),
    LOAD(transposeW, (W/64) * (H/64) * 256),
    LOAD(transposeH, (W/64) * (H/64) * 256),
    LOAD(square,   hN / 2),
    LOAD(multiply, hN / 2),
    LOAD(tailFused, hN / (2 * nH)),
    LOAD(readResidue, 64),
    LOAD(doCheck, hN / nW),
    LOAD(compare, hN / 16),
#undef LOAD

    bufData(     makeBuf(context, CL_MEM_READ_WRITE, N * sizeof(int))),
    bufCheck(    makeBuf(context, CL_MEM_READ_WRITE, N * sizeof(int))),
    bufCheck2(   makeBuf(context, BUF_RW, N * sizeof(int))),
    bufGoodData( makeBuf(context, BUF_RW, N * sizeof(int))),
    bufGoodCheck(makeBuf(context, BUF_RW, N * sizeof(int))),    
    bufTrigW(genSmallTrig(context, W, nW)),
    bufTrigH(genSmallTrig(context, H, nH)),
    bufTransTrig(genTransTrig(context, W, H)),
    bufSquareTrig(genSquareTrig(context, W, H)),
    
    buf1{makeBuf(    context, BUF_RW, bufSize)},
    buf2{makeBuf(    context, BUF_RW, bufSize)},
    buf3{makeBuf(    context, BUF_RW, bufSize)},
    bufCarry{makeBuf(context, BUF_RW, bufSize)},
    bufSmallOut(makeBuf(context, CL_MEM_READ_WRITE, 256 * sizeof(int)))
  {
    setupWeights<double>(context, bufA, bufI, W, H, E);

    fftP.setArg("out", buf1);
    fftP.setArg("A", bufA);
    fftP.setArg("smallTrig", bufTrigW);
  
    fftW.setArg("io", buf1);
    fftW.setArg("smallTrig", bufTrigW);
  
    fftH.setArg("io", buf2);
    fftH.setArg("smallTrig", bufTrigH);
  
    transposeW.setArg("in",  buf1);
    transposeW.setArg("out", buf2);
    transposeW.setArg("trig", bufTransTrig);

    transposeH.setArg("in",  buf2);
    transposeH.setArg("out", buf1);
    transposeH.setArg("trig", bufTransTrig);
  
    carryA.setArg("in", buf1);
    carryA.setArg("A", bufI);
    carryA.setArg("carryOut", bufCarry);

    carryM.setArg("in", buf1);
    carryM.setArg("A", bufI);
    carryM.setArg("carryOut", bufCarry);

    carryB.setArg("carryIn", bufCarry);
  
    square.setArg("io", buf2);
    square.setArg("bigTrig", bufSquareTrig);
  
    multiply.setArg("io", buf2);
    multiply.setArg("in", buf3);
    multiply.setArg("bigTrig", bufSquareTrig);
  
    tailFused.setArg("io", buf2);
    tailFused.setArg("smallTrig", bufTrigH);
    tailFused.setArg("bigTrig", bufSquareTrig);

    doCheck.setArg("in1", bufCheck);
    doCheck.setArg("in2", bufCheck2);
    doCheck.setArg("out", bufSmallOut);

    compare.setArg("in1", bufCheck);
    compare.setArg("in2", bufCheck2);
    uint zero = 0;
    compare.setArg("offset", zero);
    compare.setArg("out", bufSmallOut);

    readResidue.setArg("in", bufData);
    readResidue.setArg("out", bufSmallOut);
  }

  void logTimeKernels() {
    ::logTimeKernels({&fftP, &fftW, &fftH, &carryA, &carryM, &carryB, &transposeW, &transposeH, &square, &multiply, &tailFused});
  }

  void writeState(const std::vector<u32> &check, int blockSize) {
    std::vector<int> temp(N);
    expandBits(check, true, W, H, E, temp.data());
    queue.write(bufCheck, temp);
    dataFromCheck(blockSize);
  }
  
  std::vector<u32> roundtripData() { return roundtripRead(bufData); }
  std::vector<u32> roundtripCheck() { return roundtripRead(bufCheck); }
  
  void saveGood() {
    queue.copy<int>(bufData, bufGoodData, N);
    queue.copy<int>(bufCheck, bufGoodCheck, N);
  }

  void revertGood() {
    queue.copy<int>(bufGoodData, bufData, N);
    queue.copy<int>(bufGoodCheck, bufCheck, N);
  }
  
  u64 dataResidue() {
    readResidue.setArg("start", 0);
    readResidue();
    std::vector<int> readBuf = queue.read<int>(bufSmallOut, 128);
    return residueFromRaw(N, E, readBuf);
  }
  
  void updateCheck() { modMul(bufData, bufCheck, false); }
  
  // Does not change "data". Updates "check".
  bool checkAndUpdate(int blockSize) {
    modSqLoop(bufCheck, bufCheck2, blockSize, true);
    updateCheck();

    compare();
    auto readBuf1 = queue.read<int>(bufSmallOut, 2);
    bool isEqual1   = readBuf1[0];
    bool isNotZero1 = readBuf1[1];
        
    doCheck();
    auto readBuf = queue.read<int>(bufSmallOut, 2);
    bool isEqual   = readBuf[0];
    bool isNotZero = readBuf[1];
    return isEqual && isNotZero && isEqual1 && isNotZero1;
  }

  void dataLoop(int reps) { modSqLoop(bufData, bufData, reps, false); }
  
private:
  std::vector<u32> roundtripRead(Buffer &buf) {
    auto raw = queue.read<int>(buf, N);
    queue.write(buf, raw);
    return compactBits(raw.data(), W, H, E);
  }
    
  // The IBDWT convolution squaring loop with carry propagation, on 'io', done nIters times.
  // Optional multiply-by-3 at the end.
  void modSqLoop(Buffer &in, Buffer &out, int nIters, bool doMul3) {
    assert(nIters > 0);
            
    entryKerns(in);
      
    // carry args needed for coreKerns.
    carryA.setArg("out", out);
    carryB.setArg("io",  out);
    fftP.setArg("in", out);

    for (int i = 0; i < nIters - 1; ++i) { coreKerns(); }

    exitKerns(out, doMul3);
  }

  // The modular multiplication io *= in.
  void modMul(Buffer &in, Buffer &io, bool doMul3) {
    directFFT(in, buf3);
    directFFT(io, buf2);
    multiply(); // input: buf2, buf3; output: buf2.
    fftH();
    transposeH();
    exitKerns(io, doMul3);
  };

  // rebuild bufData based on bufCheck.
  void dataFromCheck(int blockSize) {
    modSqLoop(bufCheck, bufData, 1, false);
    for (int i = 0; i < blockSize - 2; ++i) {
      modMul(bufCheck, bufData, false);
      modSqLoop(bufData, bufData, 1, false);
    }
    modMul(bufCheck, bufData, true);
  }
  
  void carry() {
    fftW();
    carryA();
    carryB();
    fftP();
  }

  void tail() {
    if (useSplitTail) {
      fftH();
      square();
      fftH();      
    } else {
      tailFused();
    }
  }

  void entryKerns(Buffer &in) {
    fftP.setArg("in", in);
      
    fftP();
    transposeW();
    tail();
    transposeH();
  }

  void coreKerns() {
    carry();
    transposeW();
    tail();
    transposeH();
  }

  void exitKerns(Buffer &out, bool doMul3) {
    (doMul3 ? carryM : carryA).setArg("out", out);
    carryB.setArg("io",  out);
    
    fftW();
    doMul3 ? carryM() : carryA();
    carryB();
  }

  void directFFT(Buffer &in, Buffer &out) {
    fftP.setArg("in", in);
    transposeW.setArg("out", out);
    fftH.setArg("io", out);
      
    fftP();
    transposeW();
    fftH();
  }
};

bool checkPrime(Gpu &gpu, int W, int H, int E, cl_queue queue, cl_context context, const Args &args,
                bool *outIsPrime, u64 *outResidue, int *outNErrors) {
  const int N = 2 * W * H;
  int nErrors = 0;
  int k = 0;
  int blockSize = 0;

  {
    vector<u32> check = Checkpoint::load(E, &k, &nErrors, &blockSize);
    gpu.writeState(check, blockSize);
  }
  
  log("[%s] PRP M(%d): FFT %dK (%dx%dx2), %.2f bits/word, block %d, at iteration %d\n",
      longTimeStr().c_str(), E, N / 1024, W, H, E / float(N), blockSize, k);
  
  const int kEnd = E;
  assert(k % blockSize == 0 && k < kEnd);
  
  oldHandler = signal(SIGINT, myHandler);

  u64 res64 = gpu.dataResidue();
  if (gpu.checkAndUpdate(blockSize)) {
    log("OK initial check; %016llx\n", res64);
  } else {
    log("Error at start, will stop. %016llx\n", res64);
    return false;
  }

  gpu.saveGood();
  int goodK = k;
  int startK = k;
  Stats stats;

  // Residue at the most recent error. Used for persistent-error detection.
  // Set it to something randomly so it's not easily hit by bad luck.
  const u64 randomResidue = 0xbad0beefdeadbeefull;
  u64 errorResidue = randomResidue;
  
  Timer timer;
  while (true) {
    assert(k % blockSize == 0);

    gpu.dataLoop(std::min(blockSize, kEnd - k));
    
    if (kEnd - k <= blockSize) {
      auto words = gpu.roundtripData();
      u64 resRaw = residue(words);
      doDiv9(E, words);
      u64 resDiv = residue(words);
      words[0] = 0;
      bool isPrime = (resRaw == 9) && isAllZero(words);

      log("%s %8d / %d, %s (raw %s)\n", isPrime ? "PP" : "CC", kEnd, E, hexStr(resDiv).c_str(), hexStr(resRaw).c_str());
      
      *outIsPrime = isPrime;
      *outResidue = resDiv;
      *outNErrors = nErrors;
      int itersLeft = blockSize - (kEnd - k);
      assert(itersLeft > 0);
      gpu.dataLoop(itersLeft);
    }

    finish(queue);
    k += blockSize;
    auto delta = timer.deltaMillis();
    stats.add(delta * (1/float(blockSize)));
    
    if (stopRequested) {
      log("\nStopping, please wait..\n");
      signal(SIGINT, oldHandler);
    }

    bool doCheck = (k % 50000 == 0) || (k >= kEnd) || stopRequested || (k - startK == 2 * blockSize);
    if (!doCheck) {
      gpu.updateCheck();
      if (k % 10000 == 0) { doSmallLog(E, k, gpu.dataResidue(), stats, args.cpu); }
      continue;
    }

    u64 res = gpu.dataResidue();
    bool wouldSave = k < kEnd && ((k % 100000 == 0) || stopRequested);    
    std::vector<u32> compactCheck;
    // Read GPU state before "check" is updated in gpu.checkAndUpdate().
    if (wouldSave) { compactCheck = gpu.roundtripCheck(); }
    
    bool ok = gpu.checkAndUpdate(blockSize);
    bool doSave = wouldSave && ok;
    if (doSave) { Checkpoint::save(E, compactCheck, k, nErrors, blockSize); }
    doLog(E, k, timer.deltaMillis(), res, ok, nErrors, stats, doSave, args.cpu);
    
    if (ok) {
      if (k >= kEnd) { return true; }
      gpu.saveGood();
      goodK = k;
      errorResidue = randomResidue;
    } else {
      if (errorResidue == res) {
        log("Persistent error; will stop.\n");
        return false;
      }
      errorResidue = res;
      ++nErrors;
      gpu.revertGood();
      k = goodK;
      log("Rolled back to last good iteration %d\n", goodK);
    }
    if (args.timeKernels) { gpu.logTimeKernels(); }
    if (stopRequested) { return false; }
  }
}

cl_device_id getDevice(const Args &args) {
  cl_device_id device = nullptr;
  if (args.device >= 0) {
    cl_device_id devices[16];
    int n = getDeviceIDs(false, 16, devices);
    assert(n > args.device);
    device = devices[args.device];
  } else {
    int n = getDeviceIDs(true, 1, &device);
    if (n <= 0) {
      log("No GPU device found. See -h for how to select a specific device.\n");
      return 0;
    }
  }
  return device;
}

string valueDefine(const string &key, u32 value) {
  return key + "=" + std::to_string(value) + "u";
}

u32 modInv(u32 a, u32 m) {
  a = a % m;
  for (u32 i = 1; i < m; ++i) {
    if (a * i % m == 1) { return i; }
  }
  assert(false);
}

bool doIt(cl_device_id device, cl_context context, cl_queue queue, const Args &args, const string &AID, int E, int W, int H) {
  assert(W == 2048 || W == 4096);
  assert(H == 2048);

  int N = 2 * W * H;
  
  string configName = (N % (1024 * 1024)) ? std::to_string(N / 1024) + "K" : std::to_string(N / (1024 * 1024)) + "M";

  int nW = 8;
  int nH = H / 256;
  
  std::vector<string> defines {valueDefine("EXP", E),
      valueDefine("WIDTH", W),
      valueDefine("NW", nW),
      valueDefine("HEIGHT", H),
      valueDefine("NH", nH),
      };

  bool useLongCarry = true; // args.carry == Args::CARRY_LONG || bitsPerWord < 15;  
  bool useSplitTail = args.tail == Args::TAIL_SPLIT;
  
  log("Note: using %s carry and %s tail kernels\n",
      useLongCarry ? "long" : "short", useSplitTail ? "split" : "fused");

  string clArgs = args.clArgs;
  if (!args.dump.empty()) { clArgs += " -save-temps=" + args.dump + "/" + configName; }
    
  Holder<cl_program> program;

  bool timeKernels = args.timeKernels;

  program.reset(compile(device, context, "gpuowl", clArgs, defines, ""));
  if (!program) { return false; }
  Gpu gpu(E, W, H, program.get(), context, queue, device, timeKernels, useSplitTail);
  program.reset();
      
  bool isPrime = false;
  u64 residue = 0;
  int nErrors = 0;
  if (!checkPrime(gpu, W, H, E, queue, context, args, &isPrime, &residue, &nErrors)) { return false; }
  
  if (!(writeResult(E, isPrime, residue, AID, args.user, args.cpu, nErrors, N) && worktodoDelete(E))) { return false; }

  if (isPrime) { return false; } // Request stop if a prime is found.
  
  return true;
}

int main(int argc, char **argv) {  
  initLog();
  
  log("gpuOwL v" VERSION " GPU Mersenne primality checker\n");
  
  Args args;
  
  if (!args.parse(argc, argv)) { return -1; }

  cl_device_id device = getDevice(args);  
  if (!device) { return -1; }

  if (args.cpu.empty()) { args.cpu = getShortInfo(device); }

  std::string info = getLongInfo(device);
  log("%s\n", info.c_str());
  
  Context context{createContext(device)};
  QueueHolder queue{makeQueue(device, context.get())};

  while (true) {
    char AID[64];
    int E = worktodoReadExponent(AID);
    if (E <= 0) { break; }

    int W = (E < 153000000) ? 2048 : 4096;
    int H = 2048;
    if (!doIt(device, context.get(), queue.get(), args, AID, E, W, H)) { break; }
  }

  log("\nBye\n");
}

