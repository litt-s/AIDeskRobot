package cn.aidesk.console;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

/**
 * 开机把手机提醒接回来：用户开着提醒就说明他要"不在电脑前也能收到"，
 * 重启一次手机就静默失效是最容易被忽略的坏体验。
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            return;
        }
        SharedPreferences preferences = context.getSharedPreferences(
                AgentAlertService.PREFS, Context.MODE_PRIVATE);
        if (!preferences.getBoolean(AgentAlertService.KEY_ALERTS_ENABLED, false)) {
            return;
        }
        Intent service = new Intent(context, AgentAlertService.class);
        service.setAction(AgentAlertService.ACTION_START);
        try {
            context.startForegroundService(service);
        } catch (Exception error) {
            /* Android 12+ 对"从后台启动前台服务"有条件限制：这里被拒就安静放弃，
               用户下次打开 App 时 MainActivity 会再把它接回来。 */
            Log.w("AIDeskAlerts", "开机自启被系统拒绝：" + error);
        }
    }
}
