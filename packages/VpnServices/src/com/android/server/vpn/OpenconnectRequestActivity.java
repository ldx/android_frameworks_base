/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.server.vpn;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.DialogInterface;
import android.content.DialogInterface.OnDismissListener;
import android.content.DialogInterface.OnCancelListener;
import android.content.Intent;
import android.os.Bundle;
import android.text.method.PasswordTransformationMethod;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup.LayoutParams;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.IOException;
import java.util.Arrays;

public class OpenconnectRequestActivity extends Activity
implements OnDismissListener, OnCancelListener {

    public static final String KEY_OPENCONNECT_TITLE = "key_oc_title";
    public static final String KEY_OPENCONNECT_TEXT = "key_oc_text";
    public static final String KEY_OPENCONNECT_CHOICES = "key_oc_choices";

    private static final String TAG =
        OpenconnectRequestActivity.class.getSimpleName();

    private static final int DIALOG_CHOOSER = 0;
    private static final int DIALOG_INPUT = 1;

    private String mTitle;
    private String mText;
    private CharSequence[] mChoices;
    private boolean mEcho;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Intent intent = getIntent();

        mTitle = intent.getStringExtra(KEY_OPENCONNECT_TITLE);
        if (mTitle == null)
            return;

        mText = intent.getStringExtra(KEY_OPENCONNECT_TEXT); // can be null

        mChoices = intent.getStringArrayExtra(KEY_OPENCONNECT_CHOICES);

        if (mTitle.toLowerCase().indexOf("password") != -1)
            mEcho = false;
        else
            mEcho = true;

        Log.d(TAG, "creating dialog title " + mTitle);
        if (mChoices != null)
            Log.d(TAG, "choices " + Arrays.toString(mChoices));

        if (mChoices != null)
            showDialog(DIALOG_CHOOSER);
        else
            showDialog(DIALOG_INPUT);
    }

    @Override
    public void onDismiss(DialogInterface dialog) {
        Log.d(TAG, "dialog dismissed");
    }

    @Override
    public void onCancel(DialogInterface dialog) {
        Log.d(TAG, "dialog cancelled");
        sendResponse(null);
        this.finish();
    }

    @Override
    protected Dialog onCreateDialog(int id) {
        AlertDialog ad = null;
        AlertDialog.Builder builder;
        final View view;

        switch (id) {
            case DIALOG_CHOOSER:
                final Spinner spinner = new Spinner(this);
                ArrayAdapter<CharSequence> adapter = new ArrayAdapter(
                        this,
                        android.R.layout.simple_spinner_item,
                        mChoices);
                adapter.setDropDownViewResource(
                        android.R.layout.simple_spinner_dropdown_item);
                spinner.setAdapter(adapter);
                spinner.setSelection(0);
                view = spinner;
                break;

            case DIALOG_INPUT:
                final EditText et = new EditText(this);
                et.setTransformationMethod(new PasswordTransformationMethod());
                view = et;
                break;

            default:
                Log.e(TAG, "invalid dialog id " + id);
                return null;
        }

        builder = new AlertDialog.Builder(this)
            .setTitle(mTitle)
            .setNegativeButton(android.R.string.cancel, new
                    DialogInterface.OnClickListener() {
                        public void onClick(DialogInterface dialog, int id) {
                            String resp = null;
                            Log.d(TAG, "input cancelled");

                            sendResponse(resp);
                        }
                    })
        .setPositiveButton(android.R.string.ok,
                new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        String resp = null;
                        if (view instanceof EditText)
                            resp = ((EditText)view).getText().toString();
                        else
                            resp = mChoices[((Spinner)view).
                                    getSelectedItemPosition()].toString();
                        Log.d(TAG, "input: " + resp);

                        sendResponse(resp);
                    }
                });

        LinearLayout ll = new LinearLayout(this);
        ll.setOrientation(LinearLayout.VERTICAL);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.FILL_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(8, 4, 8, 4);

        if (mText != null) {
            TextView tv = new TextView(this);
            tv.setText(mText);
            ll.addView(tv, lp);
        }

        ll.addView(view, lp);

        builder.setView(ll);

        ad = builder.create();

        ad.setOnDismissListener(this);
        ad.setOnCancelListener(this);

        return ad;
    }

    private void sendResponse(String response) {
        Intent intent =
            new Intent(OpenconnectService.ACTION_SEND_RESPONSE);
        intent.putExtra(OpenconnectService.EXTRA_RESPONSE, response);
        sendBroadcast(intent);

        OpenconnectRequestActivity.this.finish();
    }

}
