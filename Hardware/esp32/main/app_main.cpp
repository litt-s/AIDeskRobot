#include "esp_log.h"
#include "robot_app.h"

static const char *TAG = "deskemoji";

extern "C" void app_main(void)
{
    RobotApp &app = RobotApp::Instance();
    esp_err_t err = app.Init();
    if (err == ESP_OK) {
        err = app.Start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "startup failed: %s", esp_err_to_name(err));
        app.Stop();
        app.Deinit();
    }
}
