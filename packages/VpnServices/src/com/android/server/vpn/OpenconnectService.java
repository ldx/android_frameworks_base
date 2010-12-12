/*
 * Copyright (C) 2009, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.server.vpn;

import android.net.vpn.OpenconnectProfile;
import android.security.Credentials;
import android.util.Log;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;

import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ArrayBlockingQueue;

/**
 * The service that manages a Cisco Anyconnect VPN connection.
 */
class OpenconnectService extends VpnService<OpenconnectProfile> {

    public static final String ACTION_SEND_RESPONSE = "action_send_response";
    public static final String EXTRA_RESPONSE = "extra_response";

    private static final String TAG = "OpenconnectService";

    private static final String OPENCONNECT = "openconnect";

    private static final int OPENCONNECT_REQUEST = 1;

    private DaemonProxy mDaemonProxy;

    private BlockingQueue<String> mQueue = new ArrayBlockingQueue<String>(1);

    private ResponseReceiver mResponseReceiver = null;

    @Override
    protected void connect(String serverIp, String username, String password)
    throws IOException {
        OpenconnectProfile p = getProfile();
        VpnDaemons daemons = getDaemons();

        if (p.getUserCertificate() != null)
            mDaemonProxy = daemons.startOpenconnect(p.getServerName(),
                    username, password,
                    Credentials.USER_PRIVATE_KEY + p.getUserCertificate(),
                    Credentials.USER_CERTIFICATE + p.getUserCertificate(),
                    Credentials.CA_CERTIFICATE + p.getCaCertificate());
        else
            mDaemonProxy = daemons.startOpenconnect(p.getServerName(),
                    username, password);

        String response = null;
        String req = mDaemonProxy.receiveRequest();
        while (req != null) {
            startRequestActivity(req);

            try {
                response = mQueue.take();
            } catch (InterruptedException e) {
                Log.d(TAG, "waiting for queue: " + e);
                break;
            }
            if (response == null || response.length() == 0) {
                Log.d(TAG, "response: '" + response + "', terminating loop");
                break;
            }

            sendResponse(response);

            req = mDaemonProxy.receiveRequest();
        }

        Log.d(TAG, "end of requests, closing control socket");
        mDaemonProxy.closeControlSocket();

        unregisterReceiver();

        if (response == null || response.length() == 0)
            mDaemonProxy.stop();
    }

    private void sendResponse(String response) {
        Log.d(TAG, "sending back reponse " + response);
        try {
            mDaemonProxy.sendCommand(response);
        } catch (IOException e) {
            Log.e(TAG, "sending response: sendCommand() " + e);
        }
    }

    private void registerReceiver() {
        if (mResponseReceiver != null)
            return; // BC receiver already registered

        IntentFilter filter = new IntentFilter();
        filter.addAction(OpenconnectService.ACTION_SEND_RESPONSE);
        mResponseReceiver = new ResponseReceiver();
        mContext.registerReceiver(mResponseReceiver, filter);
    }

    private void unregisterReceiver() {
        if (mResponseReceiver != null) {
            mContext.unregisterReceiver(mResponseReceiver);
            mResponseReceiver = null;
        }
    }

    private String getText(String request) {
        String[] fields = request.split("=X=", 2);
        if (fields.length == 1)
            return null;
        return fields[0];
    }

    private String getTitle(String request) {
        String[] fields = request.split("=X=", 2);
        request = fields[fields.length - 1];

        fields = request.split(":", 2);
        if (fields.length < 1) {
            return null;
        } else {
            Log.d(TAG, "Title for " + request + " is " + fields[0]);
            return fields[0];
        }
    }

    private String[] getChoices(String request) {
        String[] fields = request.split("=X=", 2);
        request = fields[fields.length - 1];

        fields = request.split(":", 2);
        if (fields.length < 2) {
            return null;
        }
        request = fields[1].trim();

        if (request.startsWith("[")) {
            fields = request.split("\\[", 2);
            if (fields.length < 2)
                return null;
            request = fields[1];
        }

        if (request.endsWith(":") && request.length() > 2) {
            request = request.substring(0, request.length() - 2);
        }

        if (request.endsWith("]")) {
            fields = request.split("\\]", 2);
            if (fields.length < 2)
                return null;
            request = fields[0];
        }

        String[] choices = null;
        if (request.length() > 0) {
            choices = request.split("\\|");
            Log.d(TAG, "choices for " + request + " are " +
                    Arrays.toString(choices) + " (" + request.length() + ")");
        }

        return choices;
    }

    private void startRequestActivity(String request) {
        Intent intent = new Intent(mContext, OpenconnectRequestActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(OpenconnectRequestActivity.KEY_OPENCONNECT_TITLE,
                getTitle(request));
        intent.putExtra(OpenconnectRequestActivity.KEY_OPENCONNECT_TEXT,
                getText(request));
        intent.putExtra(OpenconnectRequestActivity.KEY_OPENCONNECT_CHOICES,
                getChoices(request));

        registerReceiver();

        mContext.startActivity(intent);
    }

    private class ResponseReceiver extends BroadcastReceiver {

        @Override
        public void onReceive(Context context, Intent intent) {
            String response =
                intent.getStringExtra(OpenconnectService.EXTRA_RESPONSE);
            if (response == null)
                response = "";

            try {
                mQueue.put(response);
            } catch (InterruptedException e) {
                Log.d(TAG, "waiting for queue: " + e);
            }
        }

    }

}
