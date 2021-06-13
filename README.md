# BPF for Storage: An Exokernel-Inspired Approach

This repository contains instructions and source code for reproducing the micro-benchmarks in the HotOS'21 paper *BPF for Storage: An Exokernel-Inspired Approach*. [[paper]](https://dl.acm.org/doi/abs/10.1145/3458336.3465290) [[talk]](https://youtu.be/E7K1aRSy7co)

## Dependency

Operating System: Ubuntu 20.04 with modified Linux kernel 5.8.0

Disk: Intel Optane SSD P5800X

## Code Organization

* `kernel/syscall_hook.diff`: Linux kernel patch with the dispatch hook in the  syscall layer
* `kernel/nvme_driver_hook.diff`: Linux kernel patch with the dispatch look in the NVMe driver interrupt handler
* `bpf/load_bpf.sh`: Script to load BPF program into the kernel
* `bpf/bpf_loader.c`: BPF program loader
* `bpf/bpf_program.c`: BPF program running memcpy
* `bpf/Makefile`: Makefile for the BPF program
* `bench/read_baseline.cpp`: Benchmark program for baseline read()
* `bench/read_bpf.cpp`: Benchmark program for read() with BPF
* `bench/uring_baseline.cpp`: Benchmark program for baseline io_uring
* `bench/uring_bpf.cpp`: Benchmark program for io_uring with BPF
* `bench/CMakeLists.txt`: CMakeLists for the benchmark programs

## Compile Kernel

There are two different kernel patches (`syscall_hook.diff` and `nvme_driver_hook.diff`) that contain dispatch hooks in the syscall layer and the NVMe driver, respectively. To run experiments with different dispatch hooks, we need to compile and install different kernels.

First, make sure that we have all the dependencies required to build a Linux kernel. You can run the following script to install those dependencies:

```bash
# enable deb-src
sudo cp /etc/apt/sources.list /etc/apt/sources.list~
sudo sed -Ei 's/^# deb-src /deb-src /' /etc/apt/sources.list
sudo apt-get update

# install build dependency
sudo apt-get build-dep linux linux-image-$(uname -r) -y
sudo apt-get install libncurses-dev flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf fakeroot -y
```

Then, clone the Linux repository and checkout to 5.8:

```bash
git clone https://github.com/torvalds/linux.git
cd linux
git checkout tags/v5.8
```

Apply the kernel patch you need and compile the modified kernel:

```bash
git apply syscall_hook.diff  # apply nvme_driver_hook.diff instead if you want to run experiments with the dispatch hook in the NVMe driver
make localmodconfig
make deb-pkg
```

After the kernel is successfully compiled, install all the  `.deb`  files generated in the parent folder of  `linux`:

```bash
cd ..
sudo dpkg -i *.deb
```

Finally, reboot the machine and make sure that you boot into the right kernel. You can examine your current kernel by running `uname -r` and boot into another kernel using `grub-reboot` with a reboot.

## Load BPF Program

In the micro-benchmarks mentioned in the papar, we use a simple BPF program running memcpy to simulate B-Tree page parsing.

First, install the dependencies for building and loading BPF programs:

```bash
sudo apt update
sudo apt install gcc-multilib clang llvm libelf-dev libdwarf-dev -y

wget http://archive.ubuntu.com/ubuntu/pool/universe/libb/libbpf/libbpf0_0.1.0-1_amd64.deb
wget http://archive.ubuntu.com/ubuntu/pool/universe/libb/libbpf/libbpf-dev_0.1.0-1_amd64.deb
sudo dpkg -i libbpf0_0.1.0-1_amd64.deb
sudo dpkg -i libbpf-dev_0.1.0-1_amd64.deb
```

Then, run the script provided in this repository to compile and load the BPF program before running the benchmarks:

```bash
cd bpf
sudo ./load_bpf.sh
```

## Run Benchmark

First, compile the benchmark programs:

```bash
# install CMake
apt install cmake -y

# compile benchmark programs 
cd bench
mkdir build
cd build
cmake ..
make
```

Before running the benchmark, you may disable hyper-threading and CPU frequency scaling to avoid instable results. To disable hyper-threading, you can run:

```bash
sudo bash -c "echo off > /sys/devices/system/cpu/smt/control"  # need to be run again after each reboot
```

To disable CPU frequency scaling on Intel CPUs, you can:

* Add ` intel_pstate=passive intel_pstate=no_hwp` to your kernel parameters and then reboot

  * After reboot, `cat /sys/devices/system/cpu/intel_pstate/status` should show `passive` instead of `active`

* For each online CPU core, set the `scaling_governor` to `performance`, and set both `scaling_max_freq` and `scaling_min_freq` to the max frequency

  * `scaling_governor`, `scaling_max_freq`, and `scaling_min_freq` for each CPU core are available in `/sys/devices/system/cpu/cpu$CPUID/`, where `$CPUID` is the core number
  * You can find the max frequency of a CPU core in `cpuinfo_max_freq`

* Disable all C-states except for C0 state for each online CPU core

  * C-state knobs for each CPU core are available in `/sys/devices/system/cpu/cpu$CPUID/cpuidle`, where `$CPUID` is the core number

* Run the following script to disable global CPU frequency scaling and turbo boost:

  ```bash
  cd /sys/devices/system/cpu/intel_pstate
  sudo bash -c "echo 1 > no_turbo"
  sudo bash -c "echo 100 > max_perf_pct"
  sudo bash -c "echo 100 > min_perf_pct"
  ```

### read()

To run the B-Tree lookup simulation with `read()` syscall, run:

```bash
# B-Tree lookup simulation with normal read() syscall
sudo ./read_baseline <number of threads> <b-tree depth> <number of iterations> <devices, e.g. /dev/nvme0n1 /dev/nvme1n1 /dev/nvme2n1>

# B-Tree lookup simulation with read() syscall and in-kernel dispatching
sudo ./read_bpf <number of threads> <b-tree depth> <number of iterations> <devices, e.g. /dev/nvme0n1 /dev/nvme1n1 /dev/nvme2n1>
```

After the benchmark is finished, it will print the latency of each simulated b-tree lookup at nanosecond scale.

To monitor the IOPS, you can run `sar -d -p 1 3600`. Note that for `./read_bpf` with the dispatch hook in the NVMe driver, the actual IOPS is the IOPS reported by `sar` times the B-Tree depth, since `sar` only captures IOPS in the Linux block layer, while the I/O request resubmission happens in the NVMe driver in this case.

### io_uring

To run the B-Tree lookup simulation with io_uring, run:

```bash
# B-Tree lookup simulation with normal io_uring
sudo ./uring_baseline <batch size> <b-tree depth> <number of iterations> <devices, e.g. /dev/nvme0n1 /dev/nvme1n1 /dev/nvme2n1>

# B-Tree lookup simulation with io_uring and in-kernel dispatching
sudo ./uring_bpf <batch size> <b-tree depth> <number of iterations> <devices, e.g. /dev/nvme0n1 /dev/nvme1n1 /dev/nvme2n1>
```

After the benchmark is finished, it will print the latency of each simulated b-tree lookup at nanosecond scale.

To monitor the IOPS, you can run `sar -d -p 1 3600`. Note that for `./uring_bpf` with the dispatch hook in the NVMe driver, the actual IOPS is the IOPS reported by `sar` times the B-Tree depth, since `sar` only captures IOPS in the Linux block layer, while the I/O request resubmission happens in the NVMe driver in this case.

## Contact

For any questions or comments, please reach out to yuhong.zhong@columbia.edu.