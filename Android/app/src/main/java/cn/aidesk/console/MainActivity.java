package cn.aidesk.console;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Android console shell.
 *
 * The web console already implements the complete product surface. This shell keeps the same
 * origin, cookies, responsive UI and SPA behavior, while adding a native server picker for
 * cloud or LAN deployments.
 */
public class MainActivity extends Activity {
    private static final String PREFS = "aidesk_console";
    private static final String KEY_SERVER_URL = "server_url";
    private static final int REQUEST_NOTIFICATIONS = 10;

    private SharedPreferences preferences;
    private LinearLayout rootLayout;
    private FrameLayout content;
    private WebView webView;
    private String serverUrl;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        preferences = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        rootLayout = new LinearLayout(this);
        rootLayout.setOrientation(LinearLayout.VERTICAL);
        rootLayout.setFitsSystemWindows(true);
        rootLayout.setBackgroundColor(Color.parseColor("#FFF7F8FB"));
        content = new FrameLayout(this);
        rootLayout.addView(content, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(rootLayout);
        addNavigationBar();

        serverUrl = preferences.getString(KEY_SERVER_URL, "");
        /* 上次开着手机提醒就把它接回来（服务自己也带 START_STICKY）。 */
        if (preferences.getBoolean(AgentAlertService.KEY_ALERTS_ENABLED, false)) {
            startAlertService(false);
        }
        if (serverUrl.isEmpty()) {
            showServerSetup("");
        } else {
            showConsole();
        }
    }

    private void addNavigationBar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(Color.parseColor("#FFFFFFFF"));
        bar.setPadding(dp(8), dp(6), dp(8), dp(6));

        Button reload = new Button(this);
        reload.setText(R.string.reload);
        reload.setAllCaps(false);
        reload.setTextSize(14f);
        reload.setTextColor(Color.parseColor("#FF374151"));
        reload.setBackgroundColor(Color.TRANSPARENT);
        reload.setOnClickListener(v -> reloadConsole());
        bar.addView(reload, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button server = new Button(this);
        server.setText(R.string.change_server);
        server.setAllCaps(false);
        server.setTextSize(14f);
        server.setTextColor(Color.parseColor("#FF2563EB"));
        server.setBackgroundColor(Color.TRANSPARENT);
        server.setOnClickListener(v -> changeServer());
        bar.addView(server, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        rootLayout.addView(bar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(50)));
    }

    private void showServerSetup(String preset) {
        destroyWebView();

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        int side = dp(24);
        page.setPadding(side, dp(56), side, side);

        TextView title = new TextView(this);
        title.setText(R.string.setup_title);
        title.setTextSize(26f);
        title.setTextColor(Color.parseColor("#FF17203A"));
        title.setGravity(Gravity.CENTER);
        page.addView(title, matchWrap());

        TextView subtitle = new TextView(this);
        subtitle.setText(R.string.setup_subtitle);
        subtitle.setTextSize(15f);
        subtitle.setTextColor(Color.parseColor("#FF6B7280"));
        subtitle.setLineSpacing(dp(2), 1f);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = matchWrap();
        subtitleParams.setMargins(0, dp(12), 0, dp(36));
        page.addView(subtitle, subtitleParams);

        TextView label = new TextView(this);
        label.setText(R.string.server_label);
        label.setTextSize(14f);
        label.setTextColor(Color.parseColor("#FF374151"));
        page.addView(label, matchWrap());

        final EditText input = new EditText(this);
        input.setText(preset);
        input.setHint(R.string.server_hint);
        input.setSingleLine(true);
        input.setTextSize(16f);
        input.setTextColor(Color.parseColor("#FF17203A"));
        input.setHintTextColor(Color.parseColor("#FF9CA3AF"));
        input.setBackgroundColor(Color.WHITE);
        input.setPadding(dp(14), dp(12), dp(14), dp(12));
        LinearLayout.LayoutParams inputParams = matchWrap();
        inputParams.topMargin = dp(8);
        page.addView(input, inputParams);

        Button connect = new Button(this);
        connect.setText(R.string.connect);
        connect.setTextColor(Color.WHITE);
        connect.setTextSize(16f);
        connect.setAllCaps(false);
        connect.setBackgroundColor(Color.parseColor("#FF2563EB"));
        LinearLayout.LayoutParams buttonParams = matchWrap();
        buttonParams.topMargin = dp(18);
        connect.setOnClickListener(v -> {
            String candidate = normalizeUrl(input.getText().toString());
            if (candidate.isEmpty()) {
                toast(R.string.error_empty);
                return;
            }
            if (!isHttpUrl(candidate)) {
                toast(R.string.error_invalid);
                return;
            }
            preferences.edit().putString(KEY_SERVER_URL, candidate).apply();
            serverUrl = candidate;
            showConsole();
        });
        page.addView(connect, buttonParams);

        scroll.addView(page, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        replaceContent(scroll);
    }

    @SuppressLint("SetJavaScriptEnabled")
    private void showConsole() {
        destroyWebView();
        content.removeAllViews();

        webView = new WebView(this);
        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setDatabaseEnabled(true);
        settings.setLoadsImagesAutomatically(true);
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(true);
        settings.setTextZoom(100);
        settings.setMediaPlaybackRequiresUserGesture(false);
        settings.setSupportZoom(false);
        settings.setCacheMode(WebSettings.LOAD_DEFAULT);

        CookieManager cookies = CookieManager.getInstance();
        cookies.setAcceptCookie(true);
        cookies.setAcceptThirdPartyCookies(webView, true);

        webView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                String target = uri.toString();
                if (target.startsWith(serverUrl)) {
                    return false;
                }

                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, uri));
                } catch (Exception ignored) {
                    // External viewers are optional. Do not break an ordinary web redirect.
                }
                return true;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request,
                                        WebResourceError error) {
                if (request.isForMainFrame()) {
                    Toast.makeText(MainActivity.this, R.string.error_page, Toast.LENGTH_LONG).show();
                }
            }
        });
        webView.setWebChromeClient(new WebChromeClient());

        content.addView(webView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        webView.loadUrl(serverUrl);
    }

    private void replaceContent(View view) {
        content.removeAllViews();
        if (view.getLayoutParams() == null) {
            view.setLayoutParams(new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        content.addView(view);
    }

    private void destroyWebView() {
        if (webView != null) {
            webView.stopLoading();
            webView.removeAllViews();
            webView.destroy();
            webView = null;
        }
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.main_menu, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        int id = item.getItemId();
        if (id == R.id.action_reload) {
            reloadConsole();
            return true;
        }
        if (id == R.id.action_change_server) {
            changeServer();
            return true;
        }
        if (id == R.id.action_toggle_alerts) {
            toggleAlerts();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        MenuItem item = menu.findItem(R.id.action_toggle_alerts);
        if (item != null) {
            item.setTitle(preferences.getBoolean(AgentAlertService.KEY_ALERTS_ENABLED, false)
                    ? R.string.alerts_disable
                    : R.string.alerts_enable);
        }
        return super.onPrepareOptionsMenu(menu);
    }

    /**
     * 手机提醒开关。开着的时候由常驻服务每 30 秒问一次"有没有新的 agent 事件"，
     * 通知里给全标题、内容与批准选项——语音那边只报一句"软件名 + 状态"，细节看手机。
     */
    private void toggleAlerts() {
        boolean enabled = preferences.getBoolean(AgentAlertService.KEY_ALERTS_ENABLED, false);
        if (enabled) {
            preferences.edit().putBoolean(AgentAlertService.KEY_ALERTS_ENABLED, false).apply();
            stopService(new Intent(this, AgentAlertService.class));
            toast(R.string.alerts_disabled);
        } else {
            requestNotificationPermission();
            preferences.edit().putBoolean(AgentAlertService.KEY_ALERTS_ENABLED, true).apply();
            startAlertService(true);
        }
        invalidateOptionsMenu();
    }

    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return;
        }
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQUEST_NOTIFICATIONS);
        }
    }

    private void startAlertService(boolean announce) {
        Intent intent = new Intent(this, AgentAlertService.class);
        intent.setAction(AgentAlertService.ACTION_START);
        startForegroundService(intent);
        if (announce) {
            toast(R.string.alerts_enabled);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQUEST_NOTIFICATIONS && grantResults.length > 0
                && grantResults[0] != PackageManager.PERMISSION_GRANTED) {
            toast(R.string.alerts_denied);
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }
    private void reloadConsole() {
        if (webView != null) {
            webView.reload();
            return;
        }
        if (serverUrl.isEmpty()) {
            showServerSetup("");
        } else {
            showConsole();
        }
    }

    private void changeServer() {
        if (webView != null) {
            CookieManager.getInstance().flush();
        }
        showServerSetup(serverUrl);
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) {
            webView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onPause() {
        if (webView != null) {
            webView.onPause();
            CookieManager.getInstance().flush();
        }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (webView != null) {
            webView.onResume();
        }
    }

    @Override
    protected void onDestroy() {
        destroyWebView();
        super.onDestroy();
    }

    private String normalizeUrl(String value) {
        if (value == null) return "";
        String url = value.trim();
        while (url.endsWith("/")) {
            url = url.substring(0, url.length() - 1);
        }
        return url;
    }

    private boolean isHttpUrl(String value) {
        Uri uri = Uri.parse(value);
        return ("http".equals(uri.getScheme()) || "https".equals(uri.getScheme()))
                && uri.getHost() != null && !uri.getHost().trim().isEmpty();
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int value) {
        float density = getResources().getDisplayMetrics().density;
        return Math.round(value * density);
    }

    private void toast(int textRes) {
        Toast.makeText(this, textRes, Toast.LENGTH_SHORT).show();
    }
}
