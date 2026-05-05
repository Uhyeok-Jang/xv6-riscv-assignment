#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       2000  // size of file system in blocks
#define MAXPATH      128   // maximum file path name
#define USERSTACK    1     // user stack pages

// mmap protection flags.
#define PROT_READ 0x1
#define PROT_WRITE 0x2

// mmap behavior flags.
// MAP_ANONYMOUS: 0으로 채워진 anonymous page 매핑
// MAP_POPULATE: mmap 시점에 바로 physical page 할당
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE 0x2

// 각 프로세스의 mmap 영역 기준 시작 주소
// mmap의 실제 시작 주소: MMAPBASE + addr
#define MMAPBASE 0x40000000

// mmap_area 최대 개수
#define NMMAP 64
