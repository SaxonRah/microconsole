#ifndef MC_SB_H
#define MC_SB_H
int mc_sb_init(int initial_volume);
void mc_sb_set_volume(int volume);
void mc_sb_service(void);
void mc_sb_shutdown(void);
unsigned long mc_sb_frames(void);
#endif
