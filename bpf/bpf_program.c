#include <linux/bpf.h>
#include <asm-generic/types.h>
#include <bpf/bpf_helpers.h>

char _license[] SEC("license") = "GPL";

#define __inline inline __attribute__((always_inline))
#define __noinline __attribute__((noinline))
#define __nooptimize __attribute__((optnone))

#define memcpy(dest, src, n) __builtin_memcpy((dest), (src), (n))
#define memset(dest, value, n) __builtin_memset((dest), (value), (n))

SEC("prog")
__u32 main_func(struct bpf_storage *context) {
    long number = 0;
    int i = 0;
    #pragma unroll
    for (i = 0; i < (512 / sizeof(long)); ++i) {
        memcpy(&number, &context->data[i * sizeof(long)], sizeof(long));
    }
    return 0;
}
