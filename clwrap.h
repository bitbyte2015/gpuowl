// Copyright (C) 2017 Mihai Preda.

#pragma once

#include "tinycl.h"

#include <cassert>
#include <cstdio>
#include <cstdarg>

#include <string>

#define CHECK(err) { int e = err; if (e != CL_SUCCESS) { fprintf(stderr, "error %d\n", e); assert(false); }}
#define CHECK2(err, mes) { int e = err; if (e != CL_SUCCESS) { fprintf(stderr, "error %d (%s)\n", e, mes); assert(false); }}

void getInfo(cl_device_id id, int what, size_t bufSize, void *buf) {
  size_t outSize = 0;
  CHECK(clGetDeviceInfo(id, what, bufSize, buf, &outSize));
  assert(outSize <= bufSize);
}

bool getInfoMaybe(cl_device_id id, int what, size_t bufSize, void *buf) {
  return clGetDeviceInfo(id, what, bufSize, buf, NULL) == CL_SUCCESS;
}

bool getTopology(cl_device_id id, size_t bufSize, char *buf) {
  cl_device_topology_amd top;
  if (!getInfoMaybe(id, CL_DEVICE_TOPOLOGY_AMD, sizeof(top), &top)) { return false; }
  snprintf(buf, bufSize, "%x:%u.%u",
           (unsigned) (unsigned char) top.pcie.bus, (unsigned) top.pcie.device, (unsigned) top.pcie.function);
  return true;
}

int getDeviceIDs(bool onlyGPU, size_t size, cl_device_id *out) {
  cl_platform_id platforms[8];
  unsigned nPlatforms;
  CHECK(clGetPlatformIDs(8, platforms, &nPlatforms));
  
  unsigned n = 0;
  for (int i = 0; i < (int) nPlatforms && size > n; ++i) {
    unsigned delta = 0;
    CHECK(clGetDeviceIDs(platforms[i], onlyGPU ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL, size - n, out + n, &delta));
    n += delta;
  }
  return n;
}

int getNumberOfDevices() {
  cl_platform_id platforms[8];
  unsigned nPlatforms;
  CHECK(clGetPlatformIDs(8, platforms, &nPlatforms));
  
  unsigned n = 0;
  for (int i = 0; i < (int) nPlatforms; ++i) {
    unsigned delta = 0;
    CHECK(clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ALL, 0, NULL, &delta));
    n += delta;
  }
  return n;
}

std::string getDeviceName(cl_device_id id) {
  char boardName[64];
  bool hasBoardName = getInfoMaybe(id, CL_DEVICE_BOARD_NAME_AMD, sizeof(boardName), boardName);

  char topology[64];
  bool hasTopology = getTopology(id, sizeof(topology), topology);

  unsigned computeUnits;
  getInfo(id, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(computeUnits), &computeUnits);
  
  return (hasBoardName && hasTopology) ? std::string(boardName) + " " + std::to_string(computeUnits) + " @" + topology : "";
}

void getDeviceInfo(cl_device_id device, size_t infoSize, char *info) {
  char name[64], version[64];
  getInfo(device, CL_DEVICE_NAME,    sizeof(name), name);
  getInfo(device, CL_DEVICE_VERSION, sizeof(version), version);
  unsigned computeUnits, frequency;
  getInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(computeUnits), &computeUnits);
  getInfo(device, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(frequency), &frequency);

  unsigned isEcc = 0;
  CHECK(clGetDeviceInfo(device, CL_DEVICE_ERROR_CORRECTION_SUPPORT, sizeof(isEcc), &isEcc, NULL));

  std::string board = getDeviceName(device);

  if (!board.empty()) {
    snprintf(info, infoSize, "%s, %s %4uMHz", board.c_str(), name, frequency);
  } else {
    snprintf(info, infoSize, "%s, %2ux%4uMHz", name, computeUnits, frequency);
  }
}

cl_context createContext(cl_device_id device) {
  int err;
  cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
  CHECK2(err, "clCreateContext");
  return context;
}

typedef cl_command_queue cl_queue;

void release(cl_context context) { CHECK(clReleaseContext(context)); }
void release(cl_program program) { CHECK(clReleaseProgram(program)); }
void release(cl_mem buf)         { CHECK(clReleaseMemObject(buf)); }
void release(cl_queue queue)     { CHECK(clReleaseCommandQueue(queue)); }
void release(cl_kernel k)        { CHECK(clReleaseKernel(k)); }

bool dumpBinary(cl_program program, const char *fileName) {
  FILE *fo = open(fileName, "w");
  if (!fo) {
    fprintf(stderr, "Could not create file '%s'\n", fileName);
    return false;
  }
  
  size_t size;
  CHECK(clGetProgramInfo(program, CL_PROGRAM_BINARY_SIZES, sizeof(size), &size, NULL));
  char *buf = new char[size + 1];
  CHECK(clGetProgramInfo(program, CL_PROGRAM_BINARIES, sizeof(&buf), &buf, NULL));
  fwrite(buf, 1, size, fo);
  fclose(fo);
  delete[] buf;
  return true;
}

static cl_program createProgram(cl_device_id device, cl_context context, const std::string &fileName) {
  std::string stub = std::string("#include \"") + fileName + "\"\n";
  
  const char *ptr = stub.c_str();
  size_t size = stub.size();
  int err;
  cl_program program = clCreateProgramWithSource(context, 1, &ptr, &size, &err);
  CHECK2(err, "clCreateProgram");
  return program;
}

static bool build(cl_program program, cl_device_id device, const std::string &extraArgs) {
  std::string args = std::string("-I. -cl-fast-relaxed-math ") + extraArgs;
  int err = clBuildProgram(program, 1, &device, args.c_str(), NULL, NULL);
  char buf[4096];
  size_t logSize;
  clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, sizeof(buf), buf, &logSize);
  buf[logSize] = 0;
  if (logSize > 2) { fprintf(stderr, "OpenCL compilation log (error %d):\n%s\n", err, buf); }
  return (err == CL_SUCCESS);
}

cl_program compile(cl_device_id device, cl_context context, const std::string &fileName, const std::string &extraArgs) {
  cl_program program = createProgram(device, context, fileName);
  if (!program) { return program; }

  // First try CL2.0 compilation.
  if (build(program, device, std::string("-cl-std=CL2.0 ") + extraArgs)) {
    return program;
  } else if (build(program, device, extraArgs)) {
    return program;
  } else {
    release(program);
    return 0;
  }
}  
  // Other options:
  // * to output GCN ISA: -save-temps or -save-temps=prefix or -save-temps=folder/
  // * to disable all OpenCL optimization (do not use): -cl-opt-disable
  // * -cl-uniform-work-group-size
  // * -fno-bin-llvmir
  // * various: -fno-bin-source -fno-bin-amdil


cl_kernel makeKernel(cl_program program, const char *name) {
  int err;
  cl_kernel k = clCreateKernel(program, name, &err);
  CHECK2(err, name);
  return k;
}

void setArg(cl_kernel k, int pos, const auto &value) { CHECK(clSetKernelArg(k, pos, sizeof(value), &value)); }

cl_mem makeBuf(cl_context context, unsigned kind, size_t size, const void *ptr = 0) {
  int err;
  cl_mem buf = clCreateBuffer(context, kind, size, (void *) ptr, &err);
  CHECK2(err, "clCreateBuffer");
  return buf;
}

cl_queue makeQueue(cl_device_id d, cl_context c) {
  int err;
  cl_queue q = clCreateCommandQueue(c, d, 0, &err);
  CHECK2(err, "clCreateCommandQueue");
  return q;
}

void flush( cl_queue q) { CHECK(clFlush(q)); }
void finish(cl_queue q) { CHECK(clFinish(q)); }

void run(cl_queue queue, cl_kernel kernel, size_t workSize) {
  size_t groupSize = 256;
  CHECK(clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &workSize, &groupSize, 0, NULL, NULL));
}

void read(cl_queue queue, bool blocking, cl_mem buf, size_t size, void *data, size_t start = 0) {
  CHECK(clEnqueueReadBuffer(queue, buf, blocking, start, size, data, 0, NULL, NULL));
}

void write(cl_queue queue, bool blocking, cl_mem buf, size_t size, const void *data, size_t start = 0) {
  CHECK(clEnqueueWriteBuffer(queue, buf, blocking, start, size, data, 0, NULL, NULL));
}

// void copyBuf(cl_queue q, cl_mem src, cl_mem dst, size_t size) { CHECK(clEnqueueCopyBuffer(q, src, dst, 0, 0, size, 0, nullptr, nullptr)); }
