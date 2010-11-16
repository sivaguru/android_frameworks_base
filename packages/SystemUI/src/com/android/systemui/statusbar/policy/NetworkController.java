/*
 * Copyright (C) 2010 The Android Open Source Project
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

package com.android.systemui.statusbar.policy;

import java.util.ArrayList;
import java.util.List;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Binder;
import android.os.RemoteException;
import android.provider.Settings;
import android.telephony.PhoneStateListener;
import android.telephony.ServiceState;
import android.telephony.SignalStrength;
import android.telephony.TelephonyManager;
import android.util.Slog;
import android.widget.ImageView;
import android.widget.TextView;

import com.android.internal.app.IBatteryStats;
import com.android.internal.telephony.IccCard;
import com.android.internal.telephony.TelephonyIntents;
import com.android.internal.telephony.cdma.EriInfo;
import com.android.server.am.BatteryStatsService;

import com.android.systemui.R;

public class NetworkController extends BroadcastReceiver {
    // debug
    static final String TAG = "StatusBar.NetworkController";
    static final boolean DEBUG = false;

    // telephony
    boolean mHspaDataDistinguishable;
    final TelephonyManager mPhone;
    boolean mDataConnected;
    int mPhoneSignalIconId;
    int mDataIconId;
    IccCard.State mSimState = IccCard.State.READY;
    int mPhoneState = TelephonyManager.CALL_STATE_IDLE;
    int mDataState = TelephonyManager.DATA_DISCONNECTED;
    int mDataActivity = TelephonyManager.DATA_ACTIVITY_NONE;
    ServiceState mServiceState;
    SignalStrength mSignalStrength;
    int[] mDataIconList = TelephonyIcons.DATA_G[0];
 
    // wifi
    final WifiManager mWifiManager;
    boolean mWifiEnabled, mWifiConnected;
    int mWifiLevel;
    String mWifiSsid;
    int mWifiIconId;

    // data connectivity (regardless of state, can we access the internet?)
    // state of inet connection - 0 not connected, 100 connected
    private int mInetCondition = 0;
    private static final int INET_CONDITION_THRESHOLD = 50;

    // our ui
    Context mContext;
    ArrayList<ImageView> mPhoneIconViews = new ArrayList<ImageView>();
    ArrayList<ImageView> mDataIconViews = new ArrayList<ImageView>();
    ArrayList<TextView> mLabelViews = new ArrayList<TextView>();
    int mLastPhoneSignalIconId = -1;
    int mLastCombinedDataIconId = -1;
    String mLastLabel = "";
    
    // yuck -- stop doing this here and put it in the framework
    IBatteryStats mBatteryStats;

    /**
     * Construct this controller object and register for updates.
     */
    public NetworkController(Context context) {
        mContext = context;

        // telephony
        mPhone = (TelephonyManager)context.getSystemService(Context.TELEPHONY_SERVICE);
        mPhone.listen(mPhoneStateListener,
                          PhoneStateListener.LISTEN_SERVICE_STATE
                        | PhoneStateListener.LISTEN_SIGNAL_STRENGTHS
                        | PhoneStateListener.LISTEN_CALL_STATE
                        | PhoneStateListener.LISTEN_DATA_CONNECTION_STATE
                        | PhoneStateListener.LISTEN_DATA_ACTIVITY);
        mHspaDataDistinguishable = mContext.getResources().getBoolean(
                R.bool.config_hspa_data_distinguishable);
        

        // wifi
        mWifiManager = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);

        // broadcasts
        IntentFilter filter = new IntentFilter();
        filter.addAction(WifiManager.RSSI_CHANGED_ACTION);
        filter.addAction(WifiManager.WIFI_STATE_CHANGED_ACTION);
        filter.addAction(WifiManager.SUPPLICANT_STATE_CHANGED_ACTION);
        filter.addAction(WifiManager.NETWORK_STATE_CHANGED_ACTION);
        filter.addAction(TelephonyIntents.ACTION_SIM_STATE_CHANGED);
        context.registerReceiver(this, filter);

        // yuck
        mBatteryStats = BatteryStatsService.getService();
    }

    public void addPhoneIconView(ImageView v) {
        mPhoneIconViews.add(v);
    }

    public void addCombinedDataIconView(ImageView v) {
        mDataIconViews.add(v);
    }

    public void addLabelView(TextView v) {
        mLabelViews.add(v);
    }

    public void onReceive(Context context, Intent intent) {
        final String action = intent.getAction();
        if (action.equals(WifiManager.RSSI_CHANGED_ACTION)
                || action.equals(WifiManager.WIFI_STATE_CHANGED_ACTION)
                || action.equals(WifiManager.SUPPLICANT_STATE_CHANGED_ACTION)
                || action.equals(WifiManager.NETWORK_STATE_CHANGED_ACTION)) {
            updateWifiState(intent);
            refreshViews();
        } else if (action.equals(TelephonyIntents.ACTION_SIM_STATE_CHANGED)) {
            updateSimState(intent);
            updateDataIcon();
            refreshViews();
        } else if (action.equals(ConnectivityManager.CONNECTIVITY_ACTION) ||
                 action.equals(ConnectivityManager.INET_CONDITION_ACTION)) {
            updateConnectivity(intent);
            refreshViews();
        }
    }


    // ===== Telephony ==============================================================

    PhoneStateListener mPhoneStateListener = new PhoneStateListener() {
        @Override
        public void onSignalStrengthsChanged(SignalStrength signalStrength) {
            if (DEBUG) {
                Slog.d(TAG, "onSignalStrengthsChanged signalStrength=" + signalStrength);
            }
            mSignalStrength = signalStrength;
            updateTelephonySignalStrength();
            refreshViews();
        }

        @Override
        public void onServiceStateChanged(ServiceState state) {
            if (DEBUG) {
                Slog.d(TAG, "onServiceStateChanged state=" + state.getState());
            }
            mServiceState = state;
            updateTelephonySignalStrength();
            updateDataIcon();
            refreshViews();
        }

        @Override
        public void onCallStateChanged(int state, String incomingNumber) {
            if (DEBUG) {
                Slog.d(TAG, "onCallStateChanged state=" + state);
            }
            // In cdma, if a voice call is made, RSSI should switch to 1x.
            if (isCdma()) {
                updateTelephonySignalStrength();
                refreshViews();
            }
        }

        @Override
        public void onDataConnectionStateChanged(int state, int networkType) {
            if (DEBUG) {
                Slog.d(TAG, "onDataConnectionStateChanged: state=" + state 
                        + " type=" + networkType);
            }
            mDataState = state;
            updateDataNetType(networkType);
            updateDataIcon();
            refreshViews();
        }

        @Override
        public void onDataActivity(int direction) {
            if (DEBUG) {
                Slog.d(TAG, "onDataActivity: direction=" + direction);
            }
            mDataActivity = direction;
            updateDataIcon();
            refreshViews();
        }
    };

    private final void updateSimState(Intent intent) {
        String stateExtra = intent.getStringExtra(IccCard.INTENT_KEY_ICC_STATE);
        if (IccCard.INTENT_VALUE_ICC_ABSENT.equals(stateExtra)) {
            mSimState = IccCard.State.ABSENT;
        }
        else if (IccCard.INTENT_VALUE_ICC_READY.equals(stateExtra)) {
            mSimState = IccCard.State.READY;
        }
        else if (IccCard.INTENT_VALUE_ICC_LOCKED.equals(stateExtra)) {
            final String lockedReason = intent.getStringExtra(IccCard.INTENT_KEY_LOCKED_REASON);
            if (IccCard.INTENT_VALUE_LOCKED_ON_PIN.equals(lockedReason)) {
                mSimState = IccCard.State.PIN_REQUIRED;
            }
            else if (IccCard.INTENT_VALUE_LOCKED_ON_PUK.equals(lockedReason)) {
                mSimState = IccCard.State.PUK_REQUIRED;
            }
            else {
                mSimState = IccCard.State.NETWORK_LOCKED;
            }
        } else {
            mSimState = IccCard.State.UNKNOWN;
        }
    }

    private boolean isCdma() {
        return (mSignalStrength != null) && !mSignalStrength.isGsm();
    }

    private boolean isEvdo() {
        return ((mServiceState != null)
             && ((mServiceState.getRadioTechnology() == ServiceState.RADIO_TECHNOLOGY_EVDO_0)
                 || (mServiceState.getRadioTechnology() == ServiceState.RADIO_TECHNOLOGY_EVDO_A)
                 || (mServiceState.getRadioTechnology() == ServiceState.RADIO_TECHNOLOGY_EVDO_B)));
    }

    private boolean hasService() {
        if (mServiceState != null) {
            switch (mServiceState.getState()) {
                case ServiceState.STATE_OUT_OF_SERVICE:
                case ServiceState.STATE_POWER_OFF:
                    return false;
                default:
                    return true;
            }
        } else {
            return false;
        }
    }

    private int getCdmaLevel() {
        if (mSignalStrength == null) return 0;
        final int cdmaDbm = mSignalStrength.getCdmaDbm();
        final int cdmaEcio = mSignalStrength.getCdmaEcio();
        int levelDbm = 0;
        int levelEcio = 0;

        if (cdmaDbm >= -75) levelDbm = 4;
        else if (cdmaDbm >= -85) levelDbm = 3;
        else if (cdmaDbm >= -95) levelDbm = 2;
        else if (cdmaDbm >= -100) levelDbm = 1;
        else levelDbm = 0;

        // Ec/Io are in dB*10
        if (cdmaEcio >= -90) levelEcio = 4;
        else if (cdmaEcio >= -110) levelEcio = 3;
        else if (cdmaEcio >= -130) levelEcio = 2;
        else if (cdmaEcio >= -150) levelEcio = 1;
        else levelEcio = 0;

        return (levelDbm < levelEcio) ? levelDbm : levelEcio;
    }

    private int getEvdoLevel() {
        if (mSignalStrength == null) return 0;
        int evdoDbm = mSignalStrength.getEvdoDbm();
        int evdoSnr = mSignalStrength.getEvdoSnr();
        int levelEvdoDbm = 0;
        int levelEvdoSnr = 0;

        if (evdoDbm >= -65) levelEvdoDbm = 4;
        else if (evdoDbm >= -75) levelEvdoDbm = 3;
        else if (evdoDbm >= -90) levelEvdoDbm = 2;
        else if (evdoDbm >= -105) levelEvdoDbm = 1;
        else levelEvdoDbm = 0;

        if (evdoSnr >= 7) levelEvdoSnr = 4;
        else if (evdoSnr >= 5) levelEvdoSnr = 3;
        else if (evdoSnr >= 3) levelEvdoSnr = 2;
        else if (evdoSnr >= 1) levelEvdoSnr = 1;
        else levelEvdoSnr = 0;

        return (levelEvdoDbm < levelEvdoSnr) ? levelEvdoDbm : levelEvdoSnr;
    }

    private final void updateTelephonySignalStrength() {
        // Display signal strength while in "emergency calls only" mode
        if (mServiceState == null || (!hasService() && !mServiceState.isEmergencyOnly())) {
            //Slog.d(TAG, "updateTelephonySignalStrength: no service");
            if (Settings.System.getInt(mContext.getContentResolver(),
                    Settings.System.AIRPLANE_MODE_ON, 0) == 1) {
                mPhoneSignalIconId = R.drawable.stat_sys_signal_flightmode;
            } else {
                mPhoneSignalIconId = R.drawable.stat_sys_signal_null;
            }
        } else {
            if (mSignalStrength == null) {
                mPhoneSignalIconId = R.drawable.stat_sys_signal_null;
            } else if (isCdma()) {
                // If 3G(EV) and 1x network are available than 3G should be
                // displayed, displayed RSSI should be from the EV side.
                // If a voice call is made then RSSI should switch to 1x.
                int iconLevel;
                if ((mPhoneState == TelephonyManager.CALL_STATE_IDLE) && isEvdo()){
                    iconLevel = getEvdoLevel();
                } else {
                    iconLevel = getCdmaLevel();
                }
                int[] iconList;
                if (isCdmaEri()) {
                    iconList = TelephonyIcons.TELEPHONY_SIGNAL_STRENGTH_ROAMING[mInetCondition];
                } else {
                    iconList = TelephonyIcons.TELEPHONY_SIGNAL_STRENGTH[mInetCondition];
                }
                mPhoneSignalIconId = iconList[iconLevel];
            } else {
                int asu = mSignalStrength.getGsmSignalStrength();

                // ASU ranges from 0 to 31 - TS 27.007 Sec 8.5
                // asu = 0 (-113dB or less) is very weak
                // signal, its better to show 0 bars to the user in such cases.
                // asu = 99 is a special case, where the signal strength is unknown.
                int iconLevel;
                if (asu <= 2 || asu == 99) iconLevel = 0;
                else if (asu >= 12) iconLevel = 4;
                else if (asu >= 8)  iconLevel = 3;
                else if (asu >= 5)  iconLevel = 2;
                else iconLevel = 1;

                // Though mPhone is a Manager, this call is not an IPC
                int[] iconList;
                if (mPhone.isNetworkRoaming()) {
                    iconList = TelephonyIcons.TELEPHONY_SIGNAL_STRENGTH_ROAMING[mInetCondition];
                } else {
                    iconList = TelephonyIcons.TELEPHONY_SIGNAL_STRENGTH[mInetCondition];
                }
                mPhoneSignalIconId = iconList[iconLevel];
            }
        }
    }

    private final void updateDataNetType(int net) {
        switch (net) {
            case TelephonyManager.NETWORK_TYPE_EDGE:
                mDataIconList = TelephonyIcons.DATA_E[mInetCondition];
                break;
            case TelephonyManager.NETWORK_TYPE_UMTS:
                mDataIconList = TelephonyIcons.DATA_3G[mInetCondition];
                break;
            case TelephonyManager.NETWORK_TYPE_HSDPA:
            case TelephonyManager.NETWORK_TYPE_HSUPA:
            case TelephonyManager.NETWORK_TYPE_HSPA:
                if (mHspaDataDistinguishable) {
                    mDataIconList = TelephonyIcons.DATA_H[mInetCondition];
                } else {
                    mDataIconList = TelephonyIcons.DATA_3G[mInetCondition];
                }
                break;
            case TelephonyManager.NETWORK_TYPE_CDMA:
                // display 1xRTT for IS95A/B
                mDataIconList = TelephonyIcons.DATA_1X[mInetCondition];
                break;
            case TelephonyManager.NETWORK_TYPE_1xRTT:
                mDataIconList = TelephonyIcons.DATA_1X[mInetCondition];
                break;
            case TelephonyManager.NETWORK_TYPE_EVDO_0: //fall through
            case TelephonyManager.NETWORK_TYPE_EVDO_A:
            case TelephonyManager.NETWORK_TYPE_EVDO_B:
                mDataIconList = TelephonyIcons.DATA_3G[mInetCondition];
                break;
            // TODO - add support for NETWORK_TYPE_LTE and NETWORK_TYPE_EHRPD
            default:
                mDataIconList = TelephonyIcons.DATA_G[mInetCondition];
            break;
        }
    }

    boolean isCdmaEri() {
        final int iconIndex = mServiceState.getCdmaEriIconIndex();
        if (iconIndex != EriInfo.ROAMING_INDICATOR_OFF) {
            final int iconMode = mServiceState.getCdmaEriIconMode();
            if (iconMode == EriInfo.ROAMING_ICON_MODE_NORMAL
                    || iconMode == EriInfo.ROAMING_ICON_MODE_FLASH) {
                return true;
            }
        }
        return false;
    }

    private final void updateDataIcon() {
        int iconId;
        boolean visible = true;

        if (!isCdma()) {
            // GSM case, we have to check also the sim state
            if (mSimState == IccCard.State.READY || mSimState == IccCard.State.UNKNOWN) {
                if (hasService() && mDataState == TelephonyManager.DATA_CONNECTED) {
                    switch (mDataActivity) {
                        case TelephonyManager.DATA_ACTIVITY_IN:
                            iconId = mDataIconList[1];
                            break;
                        case TelephonyManager.DATA_ACTIVITY_OUT:
                            iconId = mDataIconList[2];
                            break;
                        case TelephonyManager.DATA_ACTIVITY_INOUT:
                            iconId = mDataIconList[3];
                            break;
                        default:
                            iconId = mDataIconList[0];
                            break;
                    }
                    mDataIconId = iconId;
                } else {
                    iconId = 0;
                    visible = false;
                }
            } else {
                iconId = R.drawable.stat_sys_no_sim;
            }
        } else {
            // CDMA case, mDataActivity can be also DATA_ACTIVITY_DORMANT
            if (hasService() && mDataState == TelephonyManager.DATA_CONNECTED) {
                switch (mDataActivity) {
                    case TelephonyManager.DATA_ACTIVITY_IN:
                        iconId = mDataIconList[1];
                        break;
                    case TelephonyManager.DATA_ACTIVITY_OUT:
                        iconId = mDataIconList[2];
                        break;
                    case TelephonyManager.DATA_ACTIVITY_INOUT:
                        iconId = mDataIconList[3];
                        break;
                    case TelephonyManager.DATA_ACTIVITY_DORMANT:
                    default:
                        iconId = mDataIconList[0];
                        break;
                }
            } else {
                iconId = 0;
                visible = false;
            }
        }

        // yuck - this should NOT be done by the status bar
        long ident = Binder.clearCallingIdentity();
        try {
            mBatteryStats.notePhoneDataConnectionState(mPhone.getNetworkType(), visible);
        } catch (RemoteException e) {
        } finally {
            Binder.restoreCallingIdentity(ident);
        }

        mDataIconId = iconId;
        mDataConnected = visible;
    }

    // ===== Wifi ===================================================================

    private void updateWifiState(Intent intent) {
        final String action = intent.getAction();
        if (action.equals(WifiManager.WIFI_STATE_CHANGED_ACTION)) {
            mWifiEnabled = intent.getIntExtra(WifiManager.EXTRA_WIFI_STATE,
                    WifiManager.WIFI_STATE_UNKNOWN) == WifiManager.WIFI_STATE_ENABLED;

        } else if (action.equals(WifiManager.NETWORK_STATE_CHANGED_ACTION)
                || action.equals(WifiManager.SUPPLICANT_STATE_CHANGED_ACTION)) {
            final NetworkInfo networkInfo = (NetworkInfo)
                    intent.getParcelableExtra(WifiManager.EXTRA_NETWORK_INFO);
            boolean wasConnected = mWifiConnected;
            mWifiConnected = networkInfo != null && networkInfo.isConnected();
            // If we just connected, grab the inintial signal strength and ssid
            if (mWifiConnected && !wasConnected) {
                WifiInfo info = mWifiManager.getConnectionInfo();
                if (info != null) {
                    mWifiLevel = WifiManager.calculateSignalLevel(info.getRssi(),
                            WifiIcons.WIFI_LEVEL_COUNT);
                    mWifiSsid = huntForSsid(info);
                } else {
                    mWifiLevel = 0;
                    mWifiSsid = null;
                }
            } else if (!mWifiConnected) {
                mWifiLevel = 0;
                mWifiSsid = null;
            }

        } else if (action.equals(WifiManager.RSSI_CHANGED_ACTION)) {
            if (mWifiConnected) {
                final int newRssi = intent.getIntExtra(WifiManager.EXTRA_NEW_RSSI, -200);
                mWifiLevel = WifiManager.calculateSignalLevel(newRssi, WifiIcons.WIFI_LEVEL_COUNT);
            }
        }

        updateWifiIcons();
    }

    private void updateWifiIcons() {
        if (mWifiConnected) {
            mWifiIconId = WifiIcons.WIFI_SIGNAL_STRENGTH[mInetCondition][mWifiLevel];
        } else {
            mWifiIconId = WifiIcons.WIFI_SIGNAL_STRENGTH[0][0];
        }
    }

    private String huntForSsid(WifiInfo info) {
        String ssid = info.getSSID();
        if (ssid != null) {
            return ssid;
        }
        // OK, it's not in the connectionInfo; we have to go hunting for it
        List<WifiConfiguration> networks = mWifiManager.getConfiguredNetworks();
        for (WifiConfiguration net : networks) {
            if (net.networkId == info.getNetworkId()) {
                return net.SSID;
            }
        }
        return null;
    }


    // ===== Full or limited Internet connectivity ==================================

    private void updateConnectivity(Intent intent) {
        NetworkInfo info = (NetworkInfo)(intent.getParcelableExtra(
                ConnectivityManager.EXTRA_NETWORK_INFO));
        int connectionStatus = intent.getIntExtra(ConnectivityManager.EXTRA_INET_CONDITION, 0);

        int inetCondition = (connectionStatus > INET_CONDITION_THRESHOLD ? 1 : 0);

        switch (info.getType()) {
            case ConnectivityManager.TYPE_MOBILE:
                mInetCondition = inetCondition;
                updateDataNetType(info.getSubtype());
                updateDataIcon();
                updateTelephonySignalStrength(); // apply any change in connectionStatus
                break;
            case ConnectivityManager.TYPE_WIFI:
                mInetCondition = inetCondition;
                updateWifiIcons();
                break;
        }
    }


    // ===== Update the views =======================================================

    // figure out what to show: first wifi, then 3G, then nothing
    void refreshViews() {
        Context context = mContext;

        int combinedDataIconId;
        String label;

        if (mWifiConnected) {
            if (mWifiSsid == null) {
                label = context.getString(R.string.system_panel_signal_meter_wifi_nossid);
            } else {
                label = context.getString(R.string.system_panel_signal_meter_wifi_ssid_format,
                                      mWifiSsid);
            }
            combinedDataIconId = mWifiIconId;
        } else if (mDataConnected) {
            label = context.getString(R.string.system_panel_signal_meter_data_connected);
            combinedDataIconId = mDataIconId;
        } else {
            label = context.getString(R.string.system_panel_signal_meter_disconnected);
            combinedDataIconId = 0;
        }

        int N;

        if (mLastPhoneSignalIconId != mPhoneSignalIconId) {
            mLastPhoneSignalIconId = mPhoneSignalIconId;
            N = mPhoneIconViews.size();
            for (int i=0; i<N; i++) {
                ImageView v = mPhoneIconViews.get(i);
                v.setImageResource(mPhoneSignalIconId);
            }
        }

        if (mLastCombinedDataIconId != combinedDataIconId) {
            mLastCombinedDataIconId = combinedDataIconId;
            N = mDataIconViews.size();
            for (int i=0; i<N; i++) {
                ImageView v = mDataIconViews.get(i);
                v.setImageResource(combinedDataIconId);
            }
        }

        if (!mLastLabel.equals(label)) {
            mLastLabel = label;
            N = mLabelViews.size();
            for (int i=0; i<N; i++) {
                TextView v = mLabelViews.get(i);
                v.setText(label);
            }
        }
    }
}
