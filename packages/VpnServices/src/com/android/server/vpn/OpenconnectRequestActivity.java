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
import java.util.List;
import java.util.LinkedList;

public class OpenconnectRequestActivity extends Activity
implements OnDismissListener, OnCancelListener {

    public static final String KEY_OPENCONNECT_LIST = "key_oc_list";

    private static final String TAG =
        OpenconnectRequestActivity.class.getSimpleName();

    private static final int OC_DIALOG = 0;

    private static String TITLE = "Openconnect";

    private List<OpenconnectRequest> mOptionList;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Intent intent = getIntent();
        String[] arr = intent.getStringArrayExtra(
                OpenconnectRequestActivity.KEY_OPENCONNECT_LIST);
        mOptionList = getOptions(arr);
        if (mOptionList == null || mOptionList.size() == 0) {
            Log.d(TAG, "invalid form option list");
            return;
        }

        showDialog(OC_DIALOG);
    }

    @Override
    public void onDismiss(DialogInterface dialog) {
    }

    @Override
    public void onCancel(DialogInterface dialog) {
        sendResponse(null);
        this.finish();
    }

    private List<OpenconnectRequest> getOptions(String[] optarr) {
        LinkedList<OpenconnectRequest> requests =
            new LinkedList<OpenconnectRequest>();
        for (String req : optarr) {
            try {
                OpenconnectRequest or = new OpenconnectRequest(req);
                requests.add(or);
            } catch (IOException e) {
                Log.d(TAG, "error processing " + req + ": " + e);
            }
        }

        return requests;
    }

    private String getViewText(View view) {
        String val = null;
        if (view instanceof EditText) {
            val = ((EditText)view).getText().toString();
        } else if (view instanceof Spinner) {
            Spinner s = (Spinner)view;
            String[] choices = (String[])s.getTag(R.string.vpn_openconnect_optchoices);
            val = choices[s.getSelectedItemPosition()].toString();
        }

        return val;
    }

    @Override
    protected Dialog onCreateDialog(int id) {
        final LinearLayout ll = new LinearLayout(this);
        ll.setOrientation(LinearLayout.VERTICAL);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.FILL_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(8, 4, 8, 4);

        for (OpenconnectRequest option : mOptionList) {
            // add name of option first
            TextView tv = new TextView(this);
            tv.setText(option.getLabel());
            tv.setTag(R.string.vpn_openconnect_opttype, '?');
            tv.setTag(R.string.vpn_openconnect_optname, "");
            ll.addView(tv, lp);

            // input field based on option type
            if (option.getType() == 'M') {
                tv.setText(option.getValue());
                tv.setTag(R.string.vpn_openconnect_opttype, option.getType());
                tv.setTag(R.string.vpn_openconnect_optname, option.getName());
            } else if (option.getType() == 'P') {
                EditText et = new EditText(this);
                if (option.getValue() != null)
                    et.setText(option.getValue());
                et.setTransformationMethod(new PasswordTransformationMethod());
                et.setTag(R.string.vpn_openconnect_opttype, option.getType());
                et.setTag(R.string.vpn_openconnect_optname, option.getName());
                ll.addView(et, lp);
            } else if (option.getType() == 'T') {
                EditText et = new EditText(this);
                if (option.getValue() != null)
                    et.setText(option.getValue());
                et.setTag(R.string.vpn_openconnect_opttype, option.getType());
                et.setTag(R.string.vpn_openconnect_optname, option.getName());
                ll.addView(et, lp);
            } else if (option.getType() == 'S') {
                Spinner spinner = new Spinner(this);
                ArrayAdapter<CharSequence> adapter = new ArrayAdapter(
                        this,
                        android.R.layout.simple_spinner_item,
                        option.getChoiceLabels());
                adapter.setDropDownViewResource(
                        android.R.layout.simple_spinner_dropdown_item);
                spinner.setAdapter(adapter);
                spinner.setSelection(0);
                spinner.setTag(R.string.vpn_openconnect_opttype, option.getType());
                spinner.setTag(R.string.vpn_openconnect_optname, option.getName());
                spinner.setTag(R.string.vpn_openconnect_optchoices, option.getChoiceNames());
                ll.addView(spinner, lp);
            }
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(this)
            .setTitle(TITLE)
            .setNegativeButton(android.R.string.cancel, new
                    DialogInterface.OnClickListener() {
                        public void onClick(DialogInterface dialog, int id) {
                            Log.d(TAG, "input cancelled");
                            sendResponse(null);
                        }
                    })
        .setPositiveButton(android.R.string.ok,
                new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        LinkedList<String> resplist = new LinkedList<String>();
                        int i;
                        for (i = 0; i < ll.getChildCount(); i++) {
                            View v = ll.getChildAt(i);
                            if (v == null) {
                                Log.e(TAG, "invalid view position " + i);
                                continue;
                            }
                            String value = getViewText(v);
                            if (value == null) {
                                continue;
                            }
                            char type = (Character)v.getTag(
                                R.string.vpn_openconnect_opttype);
                            String name = (String)v.getTag(
                                R.string.vpn_openconnect_optname);
                            if (type != '?')
                                resplist.add(type + " " + name + "=" + value);
                        }

                        sendResponse(resplist.toArray(new String[0]));
                    }
                });
        builder.setView(ll);

        AlertDialog ad = builder.create();

        ad.setOnDismissListener(this);
        ad.setOnCancelListener(this);

        return ad;
    }

    private void sendResponse(String[] response) {
        Intent intent =
            new Intent(OpenconnectService.ACTION_SEND_RESPONSE);
        intent.putExtra(OpenconnectService.EXTRA_RESPONSE, response);
        sendBroadcast(intent);

        OpenconnectRequestActivity.this.finish();
    }

}
