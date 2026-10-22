#include <dlfcn.h>
#include <link.h>
#include <cstdio>
#include <cmath>
int main(){
  void* h = dlopen("/home/yusuf/lhdrvrw/driver_lighthouse.so", RTLD_NOW|RTLD_LOCAL);
  if(!h){ printf("dlopen fail %s\n", dlerror()); return 1; }
  link_map* lm; dlinfo(h, RTLD_DI_LINKMAP, &lm);
  printf("base %p\n", (void*)lm->l_addr);
  using Fn = bool(*)(void*, double*, double*, double*, double*, const double*, double*, int, bool);
  auto f = (Fn)(lm->l_addr + 0x2760e0);
  double cal[7] = {0.01, 0.02, 0.003, 0.1, 0.004, 0.2, 0.005};
  const double* tab = (const double*)(lm->l_addr + 0xa3700);
  for(int axis=0; axis<2; axis++){
    double x=0.3, y=0.2, z=-2.0, out=0; double mz=-z;
    bool ok = f(nullptr, &x, &y, &mz, cal, tab, &out, axis, false);
    printf("axis %d ok=%d out=%.17g\n", axis, ok, out);
  }
}
