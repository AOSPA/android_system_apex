/*
 * Copyright (C) 2020 The Android Open Source Project
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

package com.android.tests.apex;

import static com.google.common.truth.Truth.assertThat;

import static org.junit.Assume.assumeTrue;

import android.cts.install.lib.host.InstallUtilsHost;

import com.android.internal.util.test.SystemPreparer;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.RuleChain;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;


@RunWith(DeviceJUnit4ClassRunner.class)
public class SharedLibsApexTest extends BaseHostJUnit4Test {

    private final InstallUtilsHost mHostUtils = new InstallUtilsHost(this);
    private final TemporaryFolder mTemporaryFolder = new TemporaryFolder();
    private final SystemPreparer mPreparer = new SystemPreparer(mTemporaryFolder,
            this::getDevice);

    @Rule
    public final RuleChain ruleChain = RuleChain.outerRule(mTemporaryFolder).around(mPreparer);

    private static final String TEST_BAR_APEX = "com.android.apex.test.bar.apex";
    private static final String TEST_BAR_APEX_STRIPPED = "com.android.apex.test.bar_stripped.apex";
    private static final String TEST_FOO_APEX = "com.android.apex.test.foo.apex";
    private static final String TEST_FOO_APEX_STRIPPED = "com.android.apex.test.foo_stripped.apex";
    private static final String TEST_SHAREDLIBS_APEX =
            "com.android.apex.test.sharedlibs_generated.apex";

    /**
     * Tests basic functionality of two apex packages being force-installed and the C++ binaries
     * contained in them being executed correctly.
     */
    @Test
    public void testInstallAndRunDefaultApexs() throws Exception {
        assumeTrue("Device does not support updating APEX", mHostUtils.isApexUpdateSupported());
        assumeTrue("Device requires root", getDevice().isAdbRoot());

        for (String apex : new String[]{TEST_BAR_APEX, TEST_FOO_APEX}) {
            mPreparer.pushResourceFile(apex,
                    "/system/apex/" + apex);
        }
        mPreparer.reboot();

        String runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).contains("HELLO_FOO");
        runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).contains("HELLO_BAR");
    }

    /**
     * Tests functionality of shared libraries apex: installs two apexs "stripped" of libc++.so and
     * one apex containing it and verifies that C++ binaries can run.
     */
    @Test
    public void testInstallAndRunOptimizedApexs() throws Exception {
        assumeTrue("Device does not support updating APEX", mHostUtils.isApexUpdateSupported());
        assumeTrue("Device requires root", getDevice().isAdbRoot());

        for (String apex : new String[]{
                TEST_BAR_APEX_STRIPPED,
                TEST_FOO_APEX_STRIPPED,
                TEST_SHAREDLIBS_APEX}) {
            mPreparer.pushResourceFile(apex,
                    "/system/apex/" + apex);
        }
        mPreparer.reboot();

        String runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).contains("HELLO_FOO");
        runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).contains("HELLO_BAR");
    }
}
