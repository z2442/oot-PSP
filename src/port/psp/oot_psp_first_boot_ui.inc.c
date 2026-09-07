/* Shared by the game renderer and the standalone unpacker. */
void gfx_scegu_render_first_boot_progress(uint32_t progressPermille, const char* statusMessage, bool error) {
    const int barX = 72;
    const int barY = 148;
    const int barWidth = 336;
    const int barHeight = 16;
    unsigned int accentColor;
    unsigned int accentHighlight;
    unsigned int statusColor;
    int fillWidth;
    char percentText[16];

    if (progressPermille > 1000) {
        progressPermille = 1000;
    }
    fillWidth = (int)((progressPermille * barWidth) / 1000);
    accentColor = error ? gfx_scegu_rgba(126, 55, 48, 255) : gfx_scegu_rgba(38, 92, 78, 255);
    accentHighlight = error ? gfx_scegu_rgba(188, 88, 72, 255) : gfx_scegu_rgba(72, 140, 116, 255);
    statusColor = error ? gfx_scegu_rgba(255, 210, 196, 255) : gfx_scegu_rgba(218, 224, 218, 255);

    gfx_scegu_prepare_home_menu_draw();
    gfx_scegu_draw_rect(0, 0, HOME_MENU_WIDTH, HOME_MENU_HEIGHT, gfx_scegu_rgba(7, 15, 13, 255));
    gfx_scegu_draw_rect(34, 36, 412, 200, gfx_scegu_rgba(0, 0, 0, 154));

    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 78,
                                  error ? "Asset Setup Failed" : "Preparing Game Data",
                                  0.84f, gfx_scegu_rgba(255, 255, 245, 255),
                                  gfx_scegu_rgba(0, 0, 0, 180), INTRAFONT_ALIGN_CENTER);
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 118,
                                  (statusMessage != NULL) ? statusMessage : "Starting asset setup", 0.60f,
                                  statusColor, gfx_scegu_rgba(0, 0, 0, 180), INTRAFONT_ALIGN_CENTER);

    gfx_scegu_draw_rect(barX - 4, barY - 4, barWidth + 8, barHeight + 8, gfx_scegu_rgba(0, 0, 0, 180));
    gfx_scegu_draw_rect(barX, barY, barWidth, barHeight, gfx_scegu_rgba(20, 38, 33, 255));
    if (fillWidth > 0) {
        gfx_scegu_draw_rect(barX, barY, fillWidth, barHeight, accentColor);
        gfx_scegu_draw_rect(barX, barY, fillWidth, 3, accentHighlight);
    }

    snprintf(percentText, sizeof(percentText), "%lu%%", (unsigned long)(progressPermille / 10));
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 194, percentText, 0.58f,
                                  gfx_scegu_rgba(255, 255, 245, 255), gfx_scegu_rgba(0, 0, 0, 180),
                                  INTRAFONT_ALIGN_CENTER);
    gfx_scegu_draw_home_menu_text(HOME_MENU_WIDTH / 2, 222,
                                  error ? "Check the ROM and restart" : "First launch only - do not power off",
                                  0.48f, gfx_scegu_rgba(170, 190, 180, 255), gfx_scegu_rgba(0, 0, 0, 160),
                                  INTRAFONT_ALIGN_CENTER);
}

