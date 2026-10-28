// PokeZ hash integration: unaligned and tail reads, including allocation boundaries.
#include "../runtime/nvn/tex_hash.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const size_t sizes[]={0,1,7,8,31,32,63,64,65,255,4096,65535,65536,65537,131079};
    for(unsigned s=0;s<sizeof sizes/sizeof sizes[0];s++) {
        size_t n=sizes[s]; uint8_t *a=malloc(n+1), *b=malloc(n+1);
        for(size_t j=0;j<n;j++) a[j+1]=(uint8_t)(j*31u+(j>>7));
        memcpy(b+1,a+1,n);
        uint64_t h=tex_hash_full(a+1,n), sample=tex_hash_sample(a+1,n);
        assert(h==tex_hash_full(b+1,n) && sample==tex_hash_sample(b+1,n));
        if(n<=65536) assert(h==sample);
        // All bytes for small buffers, strided plus final byte for larger buffers.
        size_t step=n<1024?1:97;
        for(size_t j=0;j<n;j+=step) {
            a[j+1]^=1; assert(tex_hash_full(a+1,n)!=h);
            if(n<=65536) assert(tex_hash_sample(a+1,n)!=sample);
            a[j+1]^=1;
        }
        if(n) { a[n]^=0x80; assert(tex_hash_full(a+1,n)!=h); }
        free(a);free(b);
    }
    puts("texture hashes: unaligned, boundary sizes, mutations and short full coverage passed");
}
