/* TJpgDec configuration for the Music app's album art. */
#define JD_SZBUF        1024    /* input buffer */
#define JD_FORMAT       0       /* 24-bit out: LVGL's copy writes B, G, R (its RGB565 packing would swap red and blue) */
#define JD_USE_SCALE    1       /* 1/2, 1/4, 1/8 while decoding */
#define JD_TBLCLIP      1
#define JD_FASTDECODE   1       /* 32-bit barrel shifter, 6 KB work area */
