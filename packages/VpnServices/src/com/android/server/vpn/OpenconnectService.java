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
import android.net.vpn.VpnManager;
import android.net.vpn.VpnState;
import android.security.Credentials;
import android.util.Log;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;

import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedList;
import java.util.List;
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

    private ResponseReceiver mResponseReceiver = null;
    private List<String>mResponseList = new LinkedList<String>();
    private boolean mCancelled = false;

    private void handleRequest(List<String> reqlist)
    throws VpnConnectingError {
        startRequestActivity(reqlist);

        synchronized(mResponseList) {
            try {
                mResponseList.wait();
            } catch (InterruptedException e) {
                Log.d(TAG, "handleRequest() wait interrupted");
            }
            if (mCancelled)
                throw new VpnConnectingError(VpnManager.VPN_ERROR_NO_ERROR);

            sendResponse(mResponseList.toArray(new String[0]));
            mResponseList.clear();
        }
    }

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

        LinkedList<String> list = new LinkedList<String>();
        String req = mDaemonProxy.receiveRequest();
        while (req != null && !req.equals("X")) { // end of control
            list.add(req);
            if (req.equals("E")) { // end of form
                handleRequest(list);
                list.clear();
            }
            req = mDaemonProxy.receiveRequest();
        }

        Log.d(TAG, "end of requests received");

        unregisterReceiver();

        setVpnStateUp(req != null);
    }

    @Override
    protected void recover() {
        onError(0);
    }

    private void sendResponse(String[] response) {
        Log.d(TAG, "sending back reponse");
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
        mContext.registerReceiver(mResponseReceiver, filter,
                "android.net.vpn.OPENCONNECT_REQUEST", null);
    }

    private void unregisterReceiver() {
        if (mResponseReceiver != null) {
            mContext.unregisterReceiver(mResponseReceiver);
            mResponseReceiver = null;
        }
    }

    private void startRequestActivity(List list) {
        Intent intent = new Intent(mContext, OpenconnectRequestActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(OpenconnectRequestActivity.KEY_OPENCONNECT_LIST,
                list.toArray(new String[0]));

        registerReceiver();

        mContext.startActivity(intent);
    }

    private class ResponseReceiver extends BroadcastReceiver {

        @Override
        public void onReceive(Context context, Intent intent) {
            String[] response =
                intent.getStringArrayExtra(OpenconnectService.EXTRA_RESPONSE);

            synchronized (mResponseList) {
                if (response == null) { // user cancelled
                    mCancelled = true;
                } else {
                    for (String r : response) {
                        mResponseList.add(r);
                    }
                }

                mResponseList.notify();
            }
        }

    }

}
