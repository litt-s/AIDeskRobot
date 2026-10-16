package cn.aidesk.console;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.util.Log;
import android.webkit.CookieManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.List;

/**
 * 手机提醒：常驻前台服务，每隔 30 秒问一次"有没有新的 agent 事件"。
 *
 * 为什么是轮询而不是推送：控制台是自己搭的服务器，没有 FCM/APNs 通道；而这类事件是低频的
 * （一次批准、一次失败、一次完成），几十秒的延迟完全可以接受。
 *
 * 凭据直接复用 WebView 的 Cookie：手机和网页是同一个账号、同一套会话，
 * 这里不再造第二套"设备令牌"。没登录（拿不到 Cookie）时静默跳过，不打扰用户。
 *
 * 通知内容按用户要求给全：**标题 + 内容 + 选项**（语音那边只报"软件名 + 状态"，
 * 因为念长句既慢又听不清）。去重用本机记录的事件 id，重启服务不会重复弹。
 */
public class AgentAlertService extends Service {
    static final String ACTION_START = "cn.aidesk.console.action.ALERT_START";
    static final String ACTION_STOP = "cn.aidesk.console.action.ALERT_STOP";
    static final String PREFS = "aidesk_console";
    static final String KEY_SERVER_URL = "server_url";
    static final String KEY_ALERTS_ENABLED = "alerts_enabled";
    static final String KEY_ALERTS_PROMPTED = "alerts_prompted";

    private static final String KEY_SEEN_IDS = "alerts_seen_ids";
    private static final String CHANNEL_EVENTS = "aidesk_agent_events";
    private static final String CHANNEL_WATCH = "aidesk_agent_watch";
    private static final int WATCH_NOTIFICATION_ID = 1001;
    private static final int SEEN_LIMIT = 200;
    private static final int MAX_PER_POLL = 5;
    private static final long POLL_INTERVAL_MS = 30_000L;
    private static final String TAG = "AIDeskAlerts";

    private SharedPreferences preferences;
    private volatile boolean running;
    /** 本次服务启动后弹过的通知条数，随心跳上报，控制台据此判断"到底有没有在干活"。 */
    private int notifiedTotal;
    private Thread worker;

    @Override
    public void onCreate() {
        super.onCreate();
        preferences = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        createChannels();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_START : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            preferences.edit().putBoolean(KEY_ALERTS_ENABLED, false).apply();
            stopSelf();
            return START_NOT_STICKY;
        }
        preferences.edit().putBoolean(KEY_ALERTS_ENABLED, true).apply();
        startForeground(WATCH_NOTIFICATION_ID, buildWatchNotification());
        if (!running) {
            running = true;
            worker = new Thread(this::loop, "agent-alerts");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        if (worker != null) {
            worker.interrupt();
            worker = null;
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void loop() {
        while (running) {
            try {
                pollOnce();
            } catch (Exception error) {
                Log.w(TAG, "轮询失败：" + error);
            }
            try {
                Thread.sleep(POLL_INTERVAL_MS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    private void pollOnce() {
        String base = preferences.getString(KEY_SERVER_URL, "");
        if (base == null || base.isEmpty()) {
            return;
        }
        String cookie = cookieFor(base);
        if (cookie == null || cookie.isEmpty()) {
            /* 心跳本身也要登录态，这种情况只能留在本机日志里；控制台会显示"从未连接"。 */
            Log.w(TAG, "没有登录态，本轮跳过（先在 App 里登录）");
            return;
        }
        String trouble = "";
        try {
            trouble = fetchOpenEvents(base, cookie);
        } catch (Exception failure) {
            trouble = String.valueOf(failure);
            Log.w(TAG, "轮询失败：" + trouble);
        }
        reportHeartbeat(base, cookie, notifiedTotal, trouble);
    }

    /** 拉一次"待处理事件"并弹通知。返回空串表示这一轮没问题，否则返回给控制台看的原因。 */
    private String fetchOpenEvents(String base, String cookie) throws Exception {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(base + "/api/agent/events?status=open").openConnection();
            connection.setRequestMethod("GET");
            connection.setConnectTimeout(8000);
            connection.setReadTimeout(10000);
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("Cookie", cookie);
            int code = connection.getResponseCode();
            if (code == 401) {
                return "登录态已失效：请在 App 里重新登录";
            }
            if (code != 200) {
                return "服务端返回 HTTP " + code;
            }
            String body = readAll(connection.getInputStream());
            JSONArray events = new JSONObject(body).optJSONArray("events");
            if (events == null) {
                return "";
            }

            List<String> seen = readSeen();
            List<String> fresh = new ArrayList<>();
            int posted = 0;
            /* 服务端按时间倒序给，这里从最老的开始弹，通知栏的顺序才和发生顺序一致。 */
            for (int index = events.length() - 1; index >= 0 && posted < MAX_PER_POLL; index--) {
                JSONObject event = events.optJSONObject(index);
                if (event == null) {
                    continue;
                }
                String id = event.optString("id", "");
                if (id.isEmpty() || seen.contains(id)) {
                    continue;
                }
                postEventNotification(event);
                fresh.add(id);
                posted++;
                notifiedTotal++;
            }
            if (!fresh.isEmpty()) {
                seen.addAll(fresh);
                while (seen.size() > SEEN_LIMIT) {
                    seen.remove(0);
                }
                writeSeen(seen);
            }
            return "";
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    /**
     * 心跳：告诉控制台"这个手机还活着、弹了几条、这一轮为什么没成"。
     * 弹不出通知时，人最需要的不是再去翻系统设置，而是能一眼看出卡在链路的哪一环。
     */
    private void reportHeartbeat(String base, String cookie, int notified, String trouble) {
        HttpURLConnection connection = null;
        try {
            byte[] payload = new JSONObject()
                    .put("label", Build.MODEL == null ? "" : Build.MODEL)
                    .put("notified", notified)
                    .put("error", trouble == null ? "" : trouble)
                    .toString()
                    .getBytes(StandardCharsets.UTF_8);
            connection = (HttpURLConnection) new URL(base + "/api/agent/watch/heartbeat").openConnection();
            connection.setRequestMethod("POST");
            connection.setConnectTimeout(8000);
            connection.setReadTimeout(10000);
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json");
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("Cookie", cookie);
            try (OutputStream out = connection.getOutputStream()) {
                out.write(payload);
            }
            connection.getResponseCode();
        } catch (Exception ignored) {
            /* 心跳失败不打扰用户：下一轮还会再来。 */
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    private void postEventNotification(JSONObject event) {
        String source = event.optString("source", "Codex");
        String kind = event.optString("kind", "finished");
        String state = "needs_confirmation".equals(kind) ? getString(R.string.alert_state_approval)
                : "failed".equals(kind) ? getString(R.string.alert_state_failed)
                : getString(R.string.alert_state_finished);
        String details = eventDetails(event);
        String title = event.optString("title", "").trim();

        Notification notification = new Notification.Builder(this, CHANNEL_EVENTS)
                .setSmallIcon(R.drawable.ic_agent_alert)
                .setContentTitle(source + " " + state)
                .setContentText(title.isEmpty() ? details : title)
                .setStyle(new Notification.BigTextStyle().bigText(details))
                .setContentIntent(openAppIntent())
                .setAutoCancel(true)
                .setCategory(Notification.CATEGORY_STATUS)
                .build();
        manager().notify(event.optString("id", String.valueOf(System.currentTimeMillis())).hashCode(), notification);
    }

    /** 每条通知都要能回答三个问题：什么事（标题）、怎么个事（内容）、我能选什么（选项）。 */
    private String eventDetails(JSONObject event) {
        StringBuilder text = new StringBuilder();
        String title = event.optString("title", "").trim();
        String detail = event.optString("detail", "").trim();
        JSONArray options = event.optJSONArray("options");
        if (!title.isEmpty()) {
            text.append(getString(R.string.alert_label_title)).append('：').append(title);
        }
        if (!detail.isEmpty()) {
            if (text.length() > 0) {
                text.append('\n');
            }
            text.append(getString(R.string.alert_label_detail)).append('：').append(detail);
        }
        if (options != null && options.length() > 0) {
            StringBuilder joined = new StringBuilder();
            for (int index = 0; index < options.length(); index++) {
                String option = options.optString(index, "").trim();
                if (option.isEmpty()) {
                    continue;
                }
                if (joined.length() > 0) {
                    joined.append(" / ");
                }
                joined.append(option);
            }
            if (joined.length() > 0) {
                if (text.length() > 0) {
                    text.append('\n');
                }
                text.append(getString(R.string.alert_label_options)).append('：').append(joined);
            }
        }
        if (text.length() == 0) {
            text.append(getString(R.string.alert_no_title));
        }
        return text.toString();
    }

    private Notification buildWatchNotification() {
        Intent stop = new Intent(this, AgentAlertService.class).setAction(ACTION_STOP);
        PendingIntent stopIntent = PendingIntent.getService(this, 1, stop,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL_WATCH)
                .setSmallIcon(R.drawable.ic_agent_alert)
                .setContentTitle(getString(R.string.alert_running_title))
                .setContentText(getString(R.string.alert_running_text))
                .setContentIntent(openAppIntent())
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null, getString(R.string.alert_stop), stopIntent).build())
                .build();
    }

    private PendingIntent openAppIntent() {
        Intent intent = new Intent(this, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        return PendingIntent.getActivity(this, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private void createChannels() {
        NotificationManager manager = manager();
        NotificationChannel events = new NotificationChannel(CHANNEL_EVENTS,
                getString(R.string.alert_channel_events), NotificationManager.IMPORTANCE_HIGH);
        events.setDescription(getString(R.string.alert_channel_events_desc));
        manager.createNotificationChannel(events);

        NotificationChannel watch = new NotificationChannel(CHANNEL_WATCH,
                getString(R.string.alert_channel_watch), NotificationManager.IMPORTANCE_MIN);
        watch.setDescription(getString(R.string.alert_channel_watch_desc));
        manager.createNotificationChannel(watch);
    }

    private NotificationManager manager() {
        return (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
    }

    private List<String> readSeen() {
        List<String> seen = new ArrayList<>();
        String raw = preferences.getString(KEY_SEEN_IDS, "");
        if (raw == null || raw.isEmpty()) {
            return seen;
        }
        try {
            JSONArray array = new JSONArray(raw);
            for (int index = 0; index < array.length(); index++) {
                seen.add(array.optString(index, ""));
            }
        } catch (Exception ignored) {
            /* 记录坏了就当没弹过：宁可重复一条，也别永远不提醒。 */
        }
        return seen;
    }

    private void writeSeen(List<String> seen) {
        JSONArray array = new JSONArray();
        for (String id : seen) {
            array.put(id);
        }
        preferences.edit().putString(KEY_SEEN_IDS, array.toString()).apply();
    }

    private String readAll(InputStream stream) throws Exception {
        StringBuilder text = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                text.append(line);
            }
        }
        return text.toString();
    }
}
