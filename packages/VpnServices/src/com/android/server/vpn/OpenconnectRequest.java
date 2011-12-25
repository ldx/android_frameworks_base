package com.android.server.vpn;

import android.util.Log;

import java.io.IOException;
import java.util.Arrays;

public class OpenconnectRequest {

    private static final String TAG =
        OpenconnectRequest.class.getSimpleName();

    private char mType;
    private String mLabel = null;
    private String mName = null;
    private String mValue = null;
    private String[] mChoiceNames = null;
    private String[] mChoiceLabels = null;

    public OpenconnectRequest(String str) throws IOException {
        if (str.length() < 1)
            throw new IOException("invalid openconnect request " + str);

        parseRequest(str);
    }

    public char getType() {
        return mType;
    }

    public String getLabel() {
        return mLabel;
    }

    public String getName() {
        return mName;
    }

    public String getValue() {
        return mValue;
    }

    public String[] getChoiceNames() {
        if (mType == 'S')
            return mChoiceNames;
        else
            return null;
    }

    public String[] getChoiceLabels() {
        if (mType == 'S')
            return mChoiceLabels;
        else
            return null;
    }

    private void parseRequest(String request) throws IOException {
        char type = request.charAt(0);
        switch (type) {
            case 'M':
                mType = 'M';
                break;
            case 'P':
                mType = 'P';
                break;
            case 'T':
                mType = 'T';
                break;
            case 'S':
                mType = 'S';
                break;
            case 'E':
                if (request.length() != 1)
                    throw new IOException("invalid openconnect request " + request);
                mType = 'E';
                return;
        }

        if (request.charAt(1) != ' ' || request.length() < 3)
            throw new IOException("invalid openconnect request " + request);

        String rest = request.substring(2);
        if (mType == 'M') {
            mValue = rest;
        } else {
            String[] opt = parseOption(rest);
            mName = opt[0];
            mLabel = opt[1];
            mValue = opt[2];
        }

        if (mType == 'S')
            parseChoices(mValue);
    }

    private String[] parseOption(String request) throws IOException {
        request = request.trim();

        String[] fields = request.split("=", -1);
        if (fields.length < 2)
            throw new IOException("invalid openconnect request " + request);

        fields[0] = fields[0].trim();
        fields[1] = fields[1].trim();

        String[] name_and_label = fields[0].split("/");
        if (fields.length != 2)
            throw new IOException("invalid openconnect request " + request);
        name_and_label[0] = name_and_label[0].trim();
        name_and_label[1] = name_and_label[1].trim();

        String[] result = { name_and_label[0], name_and_label[1], fields[1] };
        return result;
    }

    private void parseChoices(String request) {
        request = request.trim();

        String[] fields;
        if (request.startsWith("[")) {
            fields = request.split("\\[", 2);
            if (fields.length != 2)
                return;
            request = fields[1];
        }

        if (request.endsWith("]")) {
            fields = request.split("\\]", 2);
            if (fields.length < 2)
                return;
            request = fields[0];
        }

        String[] choices = null;
        if (request.length() > 0) {
            choices = request.split("\\|");
            Log.d(TAG, "choice names for " + request + " are " +
                    Arrays.toString(choices) + " (" + request.length() + ")");
        }

        if (choices != null) {
            mChoiceNames = new String[choices.length];
            mChoiceLabels = new String[choices.length];
            int i = 0;
            for (String c : choices) {
                String[] x = c.split("/");
                if (x.length != 2)
                    continue;
                mChoiceNames[i] = x[0];
                mChoiceLabels[i] = x[1];
                i++;
            }
        }
    }

}
