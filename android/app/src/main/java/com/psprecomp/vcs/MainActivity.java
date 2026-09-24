package com.psprecomp.vcs;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("vcs_android");
    }

    private static native String nativeStatus();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        TextView status = new TextView(this);
        status.setGravity(Gravity.CENTER);
        status.setPadding(32, 32, 32, 32);
        status.setTextSize(18);
        status.setText(nativeStatus());
        setContentView(status);
    }
}
