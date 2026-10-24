/* Unit test for title_map_from_ifo(): builds libdvdread IFO structures in
 * memory and checks the resulting cell/chapter map. */
#define main minidvd_main
#include "../minidvd.c"
#undef main
#include <assert.h>
#include <math.h>

static dvd_time_t T(int h, int m, int s, int fr) {   // BCD, 25 fps
    dvd_time_t t;
    t.hour = (h / 10) << 4 | h % 10; t.minute = (m / 10) << 4 | m % 10;
    t.second = (s / 10) << 4 | s % 10; t.frame_u = 0x40 | ((fr / 10) << 4 | fr % 10);
    return t;
}
#define NEAR(a, b) (fabs((a) - (b)) < 0.01)

int main(void) {
    static cell_playback_t ca[2], cb[6];
    static pgc_t pa, pb;
    static pgci_srp_t srp[2];
    static pgcit_t pgcit;
    static ifo_handle_t ifo;
    static uint8_t pmap[3] = { 1, 2, 5 };   // chapters start at cells 1, 2, 5

    total_blocks = 100000;
    // PGC 1: a 5 minute extra
    ca[0] = (cell_playback_t){0}; ca[0].first_sector = 0; ca[0].last_sector = 999; ca[0].playback_time = T(0,2,0,0);
    ca[1] = ca[0]; ca[1].first_sector = 1000; ca[1].last_sector = 1999; ca[1].playback_time = T(0,3,0,0);
    pa.nr_of_cells = 2; pa.cell_playback = ca; pa.playback_time = T(0,5,0,0);
    // PGC 2: the film, 1:58:33 = 7113 s
    for (int i = 0; i < 6; i++) cb[i] = (cell_playback_t){0};
    cb[0].first_sector = 50000; cb[0].last_sector = 59999; cb[0].playback_time = T(0,20,0,12); // 1200.48 s
    cb[1].first_sector = 2000;  cb[1].last_sector = 9999;  cb[1].playback_time = T(0,30,0,0);  // earlier on disc
    cb[2] = cb[1]; cb[2].first_sector = 10000; cb[2].last_sector = 14999; cb[2].playback_time = T(0,10,0,0);
    cb[2].block_type = BLOCK_TYPE_ANGLE_BLOCK; cb[2].block_mode = BLOCK_MODE_FIRST_CELL;      // angle 1
    cb[3] = cb[2]; cb[3].first_sector = 15000; cb[3].last_sector = 19999;
    cb[3].block_mode = BLOCK_MODE_LAST_CELL;                                                  // angle 2: skip
    cb[4].first_sector = 20000; cb[4].last_sector = 49999; cb[4].playback_time = T(0,58,32,13); // 3512.52 s
    cb[5].first_sector = 99000; cb[5].last_sector = 200000; cb[5].playback_time = T(0,0,1,0); // past the end: skip
    pb.nr_of_cells = 6; pb.cell_playback = cb; pb.playback_time = T(1,58,33,0);
    pb.nr_of_programs = 3; pb.program_map = pmap;
    srp[0].pgc = &pa; srp[1].pgc = &pb;
    pgcit.nr_of_pgci_srp = 2; pgcit.pgci_srp = srp;
    ifo.vts_pgcit = &pgcit;

    assert(title_map_from_ifo(&ifo) == 1);
    printf("cells %d, chapters %d, length %.2f s\n", ntcells, ntchap, title_len);
    for (int i = 0; i < ntcells; i++)
        printf("  cell %d: sectors %u-%u, starts %.2f s, %.2f s long\n",
               i, tcells[i].first, tcells[i].last, tcells[i].start, tcells[i].dur);
    for (int i = 0; i < ntchap; i++) printf("  chapter %d at %.2f s\n", i + 1, tchap[i]);

    assert(ntcells == 4);                                  // angle 2 + broken cell skipped
    assert(tcells[0].first == 50000 && NEAR(tcells[0].dur, 1200.48));
    assert(tcells[1].first == 2000 && NEAR(tcells[1].start, 1200.48));
    assert(tcells[2].first == 10000);                      // first angle kept
    assert(tcells[3].first == 20000 && NEAR(tcells[3].dur, 3512.52));
    assert(NEAR(title_len, 1200.48 + 1800 + 600 + 3512.52));
    assert(ntchap == 3 && NEAR(tchap[0], 0) && NEAR(tchap[1], 1200.48) && NEAR(tchap[2], 3600.48));

    // time <-> block round trips, incl. a jump into the out-of-order cell
    int c; uint32_t b = block_at_time(1500.0, &c);
    printf("t=1500 s -> block %u (cell %d) -> t=%.2f s\n", b, c, time_at_block(b));
    assert(c == 1 && NEAR(time_at_block(b), 1500.0) == 0 ? fabs(time_at_block(b) - 1500.0) < 1.0 : 1);
    title_start();
    assert(dvd_block == 50000 && cur_cell == 0);           // starts at the film, not sector 0
    printf("starts at block %u\nOK\n", dvd_block);
    return 0;
}
