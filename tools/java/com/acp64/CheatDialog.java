package com.acp64;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Build;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Locale;

/**
 * Small window for typing a code, opened by the native runtime (a secret sequence of volume keys).
 * The runtime checks the code and says whether it did something.
 */
public final class CheatDialog {
    private CheatDialog() {}

    /** 0: unknown code, 1: infinite resources on, 2: off, 3: Queen Anne's Revenge unlocked, 4: it unlocks once the game is loaded. */
    private static native int nativeSubmit(String code);

    /** Opens the window on the UI thread. May be called from any thread. */
    public static void show(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                open(activity);
            }
        });
    }

    private static boolean french() {
        return "fr".equals(Locale.getDefault().getLanguage());
    }

    private static void open(final Activity activity) {
        if (activity.isFinishing()) return;
        final boolean fr = french();
        final EditText input = new EditText(activity);
        input.setSingleLine(true);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setImeOptions(EditorInfo.IME_ACTION_DONE);
        final AlertDialog[] holder = new AlertDialog[1];
        input.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView view, int actionId, KeyEvent event) {
                boolean enter = event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                        && event.getAction() == KeyEvent.ACTION_DOWN;
                if (actionId != EditorInfo.IME_ACTION_DONE && !enter) return false;
                submit(activity, view.getText().toString(), fr);
                holder[0].dismiss();
                return true;
            }
        });
        AlertDialog.Builder builder = Build.VERSION.SDK_INT >= 21
                ? new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                : new AlertDialog.Builder(activity);
        builder.setTitle(fr ? "Code de triche" : "Cheat code");
        builder.setView(input);
        builder.setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                submit(activity, input.getText().toString(), fr);
            }
        });
        builder.setNegativeButton(android.R.string.cancel, null);
        AlertDialog dialog = builder.create();
        holder[0] = dialog;
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        dialog.show();
    }

    private static String messageFor(int result, boolean fr) {
        switch (result) {
            case 1:
                return fr ? "Ressources infinies : activées" : "Infinite resources: on";
            case 2:
                return fr ? "Ressources infinies : désactivées" : "Infinite resources: off";
            case 3:
                return fr ? "Queen Anne's Revenge débloqué : choisis-le dans NAVIRES"
                          : "Queen Anne's Revenge unlocked: pick it in your ships";
            case 4:
                return fr ? "Code accepté : le Queen Anne's Revenge sera débloqué dès que ta partie est chargée"
                          : "Code accepted: the Queen Anne's Revenge unlocks once your game is loaded";
            default:
                return fr ? "Code inconnu" : "Unknown code";
        }
    }

    private static void submit(Activity activity, String code, boolean fr) {
        Toast.makeText(activity, messageFor(nativeSubmit(code), fr), Toast.LENGTH_LONG).show();
    }

    /** Called by the native runtime when something happened later (event 1: Queen Anne's Revenge unlocked). */
    public static void notifyEvent(final Activity activity, final int event) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (event == 1) Toast.makeText(activity, messageFor(3, french()), Toast.LENGTH_LONG).show();
            }
        });
    }
}
