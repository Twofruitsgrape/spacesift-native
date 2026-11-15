#include <cstdio>
#include "content.h"
int main() {
    content::RepairLegacyEncodingsAll();
    printf("repair done\n");
    return 0;
}
