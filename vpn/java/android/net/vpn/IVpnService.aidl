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

package android.net.vpn;

import android.net.vpn.VpnProfile;

/**
 * Interface to access a VPN service.
 * {@hide}
 */
interface IVpnService {
    /**
     * Sets up a VPN connection.
     * @param profile the profile object
     * @param username the username for authentication
     * @param password the corresponding password for authentication
     * @return true if VPN is successfully connected
     */
    boolean connect(in VpnProfile profile, String username, String password);

    /**
     * Tears down the VPN connection.
     */
    void disconnect();

    /**
     * Gets the the current connection state.
     */
    String getState(in VpnProfile profile);

    /**
     * Returns the idle state.
     * @return true if the system is not connecting/connected to a VPN
     */
    boolean isIdle();
}
