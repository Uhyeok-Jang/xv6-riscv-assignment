#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    // 시스템 콜 호출
    ps(0);
    exit(0);
}