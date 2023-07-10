#include "../main.h"

struct {
    int x;
    char y;
}s;

int main(int argc, char **argv){
    printf("%d\n", sizeof(s));  // 输出8
    return 0;
}