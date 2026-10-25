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
    printf("starts at block %u\n", dvd_block);

    // ---- stream tables: 16:9 title set ----
    static vtsi_mat_t mat;
    ifo.vtsi_mat = &mat;
    mat.vts_video_attr.display_aspect_ratio = 3;          // 16:9
    mat.nr_of_vts_audio_streams = 3;
    mat.vts_audio_attr[0].audio_format = 0; mat.vts_audio_attr[0].channels = 5;  // AC3 5.1
    mat.vts_audio_attr[0].lang_type = 1; mat.vts_audio_attr[0].lang_code = ('e' << 8) | 'n';
    mat.vts_audio_attr[1].audio_format = 6; mat.vts_audio_attr[1].channels = 5;  // DTS: skip
    mat.vts_audio_attr[1].lang_type = 1; mat.vts_audio_attr[1].lang_code = ('d' << 8) | 'a';
    mat.vts_audio_attr[2].audio_format = 0; mat.vts_audio_attr[2].channels = 1;  // AC3 stereo
    mat.vts_audio_attr[2].lang_type = 1; mat.vts_audio_attr[2].lang_code = ('e' << 8) | 'n';
    mat.vts_audio_attr[2].lang_extension = 3;                                   // director's comments
    pb.audio_control[0] = 0x8000 | (0 << 8);              // logical 0 -> AC3 #0
    pb.audio_control[1] = 0x8000 | (0 << 8);              // logical 1 -> DTS #0
    pb.audio_control[2] = 0x8000 | (1 << 8);              // logical 2 -> AC3 #1
    mat.nr_of_vts_subp_streams = 3;
    mat.vts_subp_attr[0].type = 1; mat.vts_subp_attr[0].lang_code = ('d' << 8) | 'a';
    mat.vts_subp_attr[1].type = 1; mat.vts_subp_attr[1].lang_code = ('E' << 8) | 'N';  // upper case
    mat.vts_subp_attr[1].lang_extension = 2;              // large
    mat.vts_subp_attr[2].type = 1; mat.vts_subp_attr[2].lang_code = ('d' << 8) | 'e';
    // 4:3 id << 24 | wide id << 16 | letterbox << 8 | pan&scan; logical 2 not in title
    pb.subp_control[0] = 0x80000000u | (0u << 24) | (1u << 16);
    pb.subp_control[1] = 0x80000000u | (2u << 24) | (3u << 16);
    pb.subp_control[2] = 0;

    streams_from_ifo(&ifo, main_pgc);
    assert(main_pgc == &pb);
    assert(streams_known && audio_count == 2 && sub_count == 2);
    assert(audio_sid_list[0] == 0x80 && !strcmp(audio_name[0], "English, AC3 5.1"));
    assert(audio_sid_list[1] == 0x81 &&
           !strcmp(audio_name[1], "English, AC3 stereo, director's comments"));
    assert(sub_sid_list[0] == 0x21 && !strcmp(sub_name[0], "Danish"));     // wide stream ids
    assert(sub_sid_list[1] == 0x23 && !strcmp(sub_name[1], "English, large"));

    // ---- title table: a series disc ----
    // VTS 1: 4 episodes (PGC 1-4, ~45 min each) + an extra (PGC 5, 10 min)
    //        + a decoy PGC 6 claiming 1:30:00 with almost no data
    static cell_playback_t ec[6];
    static pgc_t ep[6];
    static pgci_srp_t esrp[6];
    static pgcit_t epgcit;
    static ptt_info_t eptt[6];
    static ttu_t ettu[6];
    static vts_ptt_srpt_t eptts;
    static vtsi_mat_t emat;
    static ifo_handle_t evts, evmg;
    static title_info_t eti[8];
    static tt_srpt_t ett;
    static const int mins[6] = { 47, 44, 45, 46, 10, 90 };
    for (int i = 0; i < 6; i++) {
        ec[i] = (cell_playback_t){0};
        ec[i].first_sector = 1000000 * i;
        ec[i].last_sector = 1000000 * i + (i == 5 ? 9 : 799999);  // ~300 blocks/s; decoy: 10 blocks
        ec[i].playback_time = T(mins[i] / 60, mins[i] % 60, 0, 0);
        ep[i] = (pgc_t){0};
        ep[i].nr_of_cells = 1; ep[i].cell_playback = &ec[i];
        ep[i].playback_time = ec[i].playback_time;
        esrp[i].pgc = &ep[i];
        eptt[i].pgcn = i + 1; eptt[i].pgn = 1;
        ettu[i].nr_of_ptts = 1; ettu[i].ptt = &eptt[i];
    }
    epgcit.nr_of_pgci_srp = 6; epgcit.pgci_srp = esrp;
    eptts.nr_of_srpts = 6; eptts.title = ettu;
    evts.vts_pgcit = &epgcit; evts.vts_ptt_srpt = &eptts; evts.vtsi_mat = &emat;
    vts_ifo[1] = &evts;                   // what ifoOpen() would return
    vts_blocks[1] = 6000000;
    // title table order: extra, ep1, ep2, decoy, ep3, ep4, ep2 again (duplicate)
    static const int order[7] = { 5, 1, 2, 6, 3, 4, 2 };
    for (int i = 0; i < 7; i++) {
        eti[i] = (title_info_t){0};
        eti[i].title_set_nr = 1; eti[i].vts_ttn = order[i]; eti[i].nr_of_ptts = 1;
    }
    ett.nr_of_srpts = 7; ett.title = eti;
    evmg.tt_srpt = &ett;

    titles_from_vmg(&evmg);
    int start = titles_choose(1);
    titles_print(start);
    assert(ntitles == 5);                 // decoy and duplicate dropped
    assert(titles[0].pgcn == 5 && titles[1].pgcn == 1 && titles[4].pgcn == 4);
    assert(series && start == 1);         // starts at episode 1, not the extra
    assert(next_series_title(1) == 2 && next_series_title(3) == 4 && next_series_title(4) == -1);
    assert(next_series_title(0) == 1);    // after the extra: the episodes

    // ---- copy protection: the film plus decoy versions of it ----
    // real film: 28 cells; decoys reuse them and add junk cells of their own
    {
        static cell_playback_t pc[4][40];
        static pgc_t pp[14];
        static pgci_srp_t psrp[14];
        static pgcit_t ppgcit;
        static ptt_info_t pptt[14];
        static ttu_t pttu[14];
        static vts_ptt_srpt_t pptts;
        static vtsi_mat_t pmat;
        static ifo_handle_t pvts, pvmg;
        static title_info_t pti[14];
        static tt_srpt_t ptt;
        // real cells: 28 x 70000 sectors from 0; junk cells from 3000000
        int n[4] = { 0, 0, 0, 0 };
        uint32_t junk = 3000000;
        #define CELL(t, first, last, sec) do { cell_playback_t *c_ = &pc[t][n[t]++]; \
            *c_ = (cell_playback_t){0}; c_->first_sector = (first); c_->last_sector = (last); \
            c_->playback_time = T(0, (sec) / 60, (sec) % 60, 0); } while (0)
        for (int r = 0; r < 28; r++) {
            CELL(0, r * 70000, r * 70000 + 69999, 216);                      // film
            CELL(1, r * 70000, r * 70000 + 69999, 216);                      // decoy A
            if (r % 5 == 2) { CELL(1, junk, junk + 4999, 36); junk += 5000; } //   + junk
            CELL(2, r * 70000, r * 70000 + 69999, 216);                      // decoy B
            if (r == 10 || r == 20) { CELL(2, junk, junk + 999, 1); junk += 1000; }
            if (r < 25) CELL(3, r * 70000, r * 70000 + 69999, 216);          // decoy C
        }
        CELL(3, junk, junk + 2999, 20); junk += 3000;
        #undef CELL
        static const int lens[4][3] = { {1,40,48}, {1,44,24}, {1,40,50}, {1,30,20} };
        for (int t = 0; t < 4; t++) {
            pp[t] = (pgc_t){0};
            pp[t].nr_of_cells = n[t]; pp[t].cell_playback = pc[t];
            pp[t].playback_time = T(lens[t][0], lens[t][1], lens[t][2], 0);
        }
        // 9 short extras in their own area
        static cell_playback_t xc[9];
        for (int x = 0; x < 9; x++) {
            xc[x] = (cell_playback_t){0};
            xc[x].first_sector = 4000000 + x * 50000; xc[x].last_sector = xc[x].first_sector + 39999;
            xc[x].playback_time = T(0, 2, 0, 0);
            pp[4 + x] = (pgc_t){0};
            pp[4 + x].nr_of_cells = 1; pp[4 + x].cell_playback = &xc[x];
            pp[4 + x].playback_time = xc[x].playback_time;
        }
        for (int i = 0; i < 13; i++) {
            psrp[i].pgc = &pp[i];
            pptt[i].pgcn = i + 1; pptt[i].pgn = 1;
            pttu[i].nr_of_ptts = 1; pttu[i].ptt = &pptt[i];
        }
        ppgcit.nr_of_pgci_srp = 13; ppgcit.pgci_srp = psrp;
        pptts.nr_of_srpts = 13; pptts.title = pttu;
        pvts.vts_pgcit = &ppgcit; pvts.vts_ptt_srpt = &pptts; pvts.vtsi_mat = &pmat;
        vts_ifo[1] = &pvts;
        vts_blocks[1] = 5000000;
        // title table like the user's disc: film first, extras, decoys last
        static const int porder[13] = { 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 2, 3, 4 };
        for (int i = 0; i < 13; i++) {
            pti[i] = (title_info_t){0};
            pti[i].title_set_nr = 1; pti[i].vts_ttn = porder[i]; pti[i].nr_of_ptts = 18;
        }
        ptt.nr_of_srpts = 13; ptt.title = pti;
        pvmg.tt_srpt = &ptt;
        ntitles = 0;
        titles_from_vmg(&pvmg);
        int pstart = titles_choose(1);
        titles_print(pstart);
        assert(ntitles == 13);
        assert(pstart == 0);                              // the real film (title 1)
        assert(!series && next_series_title(0) == -1);    // no play-on into decoys
        assert(titles[10].version_of == 0 && titles[11].version_of == 0 && titles[12].version_of == 0);
    }

    // a movie disc: one long title + extras -> no series, start at the film
    ntitles = 0;
    titles[ntitles++] = (dvdtitle_t){ 1, 1, NULL, 6300, 20 };
    titles[ntitles++] = (dvdtitle_t){ 1, 2, NULL, 600, 1 };
    titles[ntitles++] = (dvdtitle_t){ 1, 3, NULL, 900, 1 };
    start = titles_choose(1);
    assert(!series && start == 0 && next_series_title(0) == -1);
    printf("OK\n");
    return 0;
}
