/*
 * Mario Smarduch - Samsung - Open Source Group  
 * 9/18/2014
 *
 * Description: 
 *
 * Simple dirty page logging program, does migration from qemu shared memory
 * device mapped into Guest address space to a file, but could be to a socket
 * as well. The other end is implemented in qemu, migratev7/8 - periodically
 * requests qemu to get dirty bitmap from kernel and write to a file migrate
 * can read. Based on dirty bits set, pages are copied from shared memory
 * device to disk file. Once Guest dirty process exits, migrate does a final
 * sweep of dirty bitmap and does a final copy of pages.
 * 
 * Expects shared memory segment at 0x20000000 size 8MB
 *
 * For armv8 Works with 4kb and 64kb pages
 *
 * See howto for more details.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#define REGSIZE 0x800000
#define RATELIMIT 10 
unsigned char *page_buf;
unsigned long copy_count;
struct sync_header {
      unsigned long r2r, p2r;
};
unsigned char *dirty_bitmap;
unsigned long page_size;

struct sync_header hdr;

/* After logging is enabled pre-copy segement memory then take page deltas. */
void pre_copy(int fd_src, int fd_dst, unsigned long size)
{
    int cnt = size/page_size, i;
    int ret;

    printf("pre-copy region\n");
    for(i=0; i<cnt; i++) {
	if((ret=read(fd_src, page_buf, page_size)) != page_size) {
		printf("ERR: source read != PAGE SIZE = %d\n", ret);
		exit(-1);
	}

	if((ret=write(fd_dst, page_buf, page_size)) != page_size) {
		printf("ERR: dest write != PAGE SIZE = %d\n", ret);
		exit(-1);
	}
    }
}

int migrate_delta(int fd_bmap, int fd_src, int fd_dst, int size)
{
    struct sync_header hdr;
    int bmap_size = REGSIZE/page_size/8;
    int i, cnt;
    unsigned long ofst;
    unsigned char tmp;
    int ret = 0, count=0; 
    static int ratelimit=RATELIMIT, printcnt=0;;

    
    lseek(fd_bmap, sizeof(hdr), SEEK_SET);
    if(read(fd_bmap, dirty_bitmap, bmap_size) != bmap_size) {
	printf("ERR: size of dirty bitmap != %d\n", bmap_size);
	exit(-1);
    } 

    /* scan dirty bitmap copy pages */
    for(i=0; i<bmap_size; i++) {
	if(!dirty_bitmap[i]) continue;
        ret = 1;
        tmp = dirty_bitmap[i];
	cnt=0;
	while(tmp) {
	    if(tmp & 1) {
		ofst = (i*8 + cnt) * page_size;
		
		lseek(fd_src, ofst, SEEK_SET);
		if((ret=read(fd_src, page_buf, page_size)) != page_size) {
		    printf("ERR: source read != PAGE SIZE = %d\n", ret);
		    exit(-1);
		}

		lseek(fd_dst, ofst, SEEK_SET);
		if((ret=write(fd_dst, page_buf, page_size)) != page_size) {
		    printf("ERR: dest write != PAGE SIZE = %d\n", ret);
		    exit(-1);
		}
		count++;
	    }	
	    cnt++;
	    tmp >>= 1;
	}
    }
    if(!ratelimit--) {
	printcnt++;
    	printf("found %d dirty pages page size = %s, t+%d\n", count, 
		page_size == 4096 ? "4k" : "64k", printcnt*RATELIMIT);
	ratelimit = RATELIMIT;
    }
    /* Total pages migrated */
    copy_count += count;
    return ret;
}


main()
{
    char buf[100];
    int fd_src, fd_dst, fd_bmap;
    int ret;
    
    page_size = getpagesize();
    printf("page_size = %s\n", page_size == 0x1000 ? "4k" : "64k");
 
    page_buf = malloc(page_size);
    if(page_buf == NULL) {
	perror("couldn't allocate page buffer\n");
	exit(-1);
    }

    printf("bitmap size = %d\n", REGSIZE/page_size/8);
    dirty_bitmap = malloc(REGSIZE/page_size/8);
    if(dirty_bitmap == NULL) {
	perror("couldn't allocate dirty_bitmap\n");
	exit(-1);
    }
    
    // Uncoment if /dev/shm path doesn't work, depends how FSs are backed
    // sprintf(buf, "/tmp/aeshmem"
    sprintf(buf, "/dev/shm/aeshmem");
    fd_src =  open(buf, O_RDWR);
    if(fd_src < 0) {
        perror("Failed to open /dev/shm/aeshmem\n");
	exit(-1);
    }

    sprintf(buf, "/home/root/dest_aeshmem");
    fd_dst =  open(buf, O_RDWR);
    if(fd_dst < 0) {
        perror("Failed to open /home/root/dest_aeshmem\n");
	exit(-1);
    }

    sprintf(buf, "/home/root/dirty_bitmap");
    fd_bmap = open(buf, O_RDWR);
    if(fd_bmap < 0) {
        perror("Failed to open /home/root/dirty_bitmap\n");
        exit(-1);
    }
   
    pre_copy(fd_src, fd_dst, REGSIZE);
   
    /* enable qemu to write out dirty bitmap */
    hdr.r2r=1; 
    lseek(fd_bmap, 0, SEEK_SET);
    write(fd_bmap, &hdr.r2r, sizeof(hdr.r2r));

    /* wait a little between test runs, otherwise you exit prematurely */
    sleep(1);
    while(1) {
	lseek(fd_bmap, 0, SEEK_SET);
    	read(fd_bmap, &hdr, sizeof(hdr));

	/* Wait on qemu until it's done copying the dirty bitmap */
    	if(hdr.p2r == 0)
	    goto stall;

        /* copy delta, if bit map is null then we're done */
	ret = migrate_delta(fd_bmap, fd_src, fd_dst, REGSIZE);
	if(ret == 0) {
		printf("migration complete copied %d %s pages\n",
			copy_count, page_size == 4096 ? "4k" : "64k");
		exit(0);
	}
        
        /* enable qemu to update dirty bitmap */
	hdr.r2r = 1;
	hdr.p2r = 0;
	lseek(fd_bmap,0, SEEK_SET);
	write(fd_bmap, &hdr, sizeof(hdr));
	
stall:
	/* wait around for qemu to get next update */
	poll(NULL,0,100);
    }
}
