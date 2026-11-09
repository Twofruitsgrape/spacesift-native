// 单独测 rules::PathSize，排除界面和线程因素
#include <cstdio>
#include <ctime>
#include <windows.h>

#include "../src/rules.h"

int main() {
    const char* paths[] = {
        "C:\\Users\\ALICE\\AppData\\Local\\pnpm\\store",
        "C:\\Windows\\WinSxS\\Backup",
        "C:\\$Recycle.Bin",
    };
    for (const char* p : paths) {
        clock_t t0 = clock();
        uint64_t files = 0;
        uint64_t sz = rules::PathSize(p, false, &files, 3000);
        double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("%-45s  %8.1f MB  %7llu files  %.2f s\n", p, sz / 1048576.0,
               (unsigned long long)files, sec);
    }
    return 0;
}
