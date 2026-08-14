#ifndef __PRELOADER_H__
#define __PRELOADER_H__

// Sistema de Precarga Total de Assets en RAM y Atlas GPU
// Muestra una pantalla de carga nativa y precarga todos los recursos de disco
void zenonia_preloader_run(void);
void osd_show_message(const char *msg, int duration_frames);
void osd_draw(void);

#endif // __PRELOADER_H__
