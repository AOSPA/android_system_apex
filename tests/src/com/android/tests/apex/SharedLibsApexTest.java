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

    enum ApexName {
        FOO,
        BAR,
        BAZ,
        SHAREDLIBS
    }

    enum ApexVersion {
        ONE,
        TWO
    }

    enum ApexType {
        DEFAULT,
        STRIPPED
    }

    enum SharedLibsVersion {
        X,
        Y
    }

    /**
     * Utility function to generate test apex names in the form e.g.:
     *   "com.android.apex.test.bar.v1.libvX.apex"
     */
    private String getTestApex(ApexName apexName, ApexType apexType, ApexVersion apexVersion,
            SharedLibsVersion sharedLibsVersion) {
        StringBuilder ret = new StringBuilder();
        ret.append("com.android.apex.test.");
        switch(apexName) {
            case FOO:
                ret.append("foo");
                break;
            case BAR:
                ret.append("bar");
                break;
            case BAZ:
                ret.append("baz");
                break;
            case SHAREDLIBS:
                ret.append("sharedlibs_generated");
                break;
        }

        switch(apexType) {
            case STRIPPED:
                ret.append("_stripped");
                break;
            case DEFAULT:
                break;
        }

        switch(apexVersion) {
            case ONE:
                ret.append(".v1");
                break;
            case TWO:
                ret.append(".v2");
                break;
        }

        switch(sharedLibsVersion) {
            case X:
                ret.append(".libvX.apex");
                break;
            case Y:
                ret.append(".libvY.apex");
                break;
        }

        return ret.toString();
    }

    /**
     * Tests basic functionality of two apex packages being force-installed and the C++ binaries
     * contained in them being executed correctly.
     */
    @Test
    public void testInstallAndRunDefaultApexs() throws Exception {
        assumeTrue("Device does not support updating APEX", mHostUtils.isApexUpdateSupported());
        assumeTrue("Device requires root", getDevice().isAdbRoot());

        for (String apex : new String[]{
                getTestApex(ApexName.BAR, ApexType.DEFAULT, ApexVersion.ONE, SharedLibsVersion.X),
                getTestApex(ApexName.FOO, ApexType.DEFAULT, ApexVersion.ONE, SharedLibsVersion.X),
        }) {
            mPreparer.pushResourceFile(apex,
                    "/system/apex/" + apex);
        }
        mPreparer.reboot();

        getDevice().disableAdbRoot();
        String runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).isEqualTo("FOO_VERSION_1 SHARED_LIB_VERSION_X");
        runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).isEqualTo("BAR_VERSION_1 SHARED_LIB_VERSION_X");

        mPreparer.stageMultiplePackages(
            new String[]{
                getTestApex(ApexName.BAR, ApexType.DEFAULT, ApexVersion.TWO, SharedLibsVersion.Y),
                getTestApex(ApexName.FOO, ApexType.DEFAULT, ApexVersion.TWO, SharedLibsVersion.Y),
            },
            new String[] {
                "com.android.apex.test.bar",
                "com.android.apex.test.foo",
            }).reboot();

        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).isEqualTo("FOO_VERSION_2 SHARED_LIB_VERSION_Y");
        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).isEqualTo("BAR_VERSION_2 SHARED_LIB_VERSION_Y");
    }

    /**
     * Tests functionality of shared libraries apex: installs two apexs "stripped" of libc++.so and
     * one apex containing it and verifies that C++ binaries can run.
     */
    @Test
    public void testInstallAndRunOptimizedApexs() throws Exception {
        assumeTrue("Device does not support updating APEX", mHostUtils.isApexUpdateSupported());
        assumeTrue("Device requires root", getDevice().isAdbRoot());

        // Pre-installed on /system:
        //   package bar version 1 using library version X
        //   package foo version 1 using library version X
        //   package sharedlibs version 1 exporting library version X

        for (String apex : new String[]{
                getTestApex(ApexName.BAR, ApexType.STRIPPED, ApexVersion.ONE, SharedLibsVersion.X),
                getTestApex(ApexName.FOO, ApexType.STRIPPED, ApexVersion.ONE, SharedLibsVersion.X),
                getTestApex(ApexName.SHAREDLIBS, ApexType.DEFAULT, ApexVersion.ONE,
                    SharedLibsVersion.X),
        }) {
            mPreparer.pushResourceFile(apex,
                    "/system/apex/" + apex);
        }
        mPreparer.reboot();

        getDevice().disableAdbRoot();
        String runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).isEqualTo("FOO_VERSION_1 SHARED_LIB_VERSION_X");
        runAsResult = getDevice().executeShellCommand(
                "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).isEqualTo("BAR_VERSION_1 SHARED_LIB_VERSION_X");

        // Updated packages (installed on /data/apex/active):
        //   package bar version 2 using library version Y
        //   package foo version 2 using library version Y
        //   package sharedlibs version 2 exporting library version Y

        mPreparer.stageMultiplePackages(
            new String[]{
                getTestApex(ApexName.BAR, ApexType.STRIPPED, ApexVersion.TWO, SharedLibsVersion.Y),
                getTestApex(ApexName.FOO, ApexType.STRIPPED, ApexVersion.TWO, SharedLibsVersion.Y),
                getTestApex(ApexName.SHAREDLIBS, ApexType.DEFAULT, ApexVersion.TWO,
                    SharedLibsVersion.Y),
            },
            new String[] {
                "com.android.apex.test.bar",
                "com.android.apex.test.foo",
                "com.android.apex.test.sharedlibs",
            }).reboot();

        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).isEqualTo("FOO_VERSION_2 SHARED_LIB_VERSION_Y");
        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).isEqualTo("BAR_VERSION_2 SHARED_LIB_VERSION_Y");

        // Assume that an OTA now adds a package baz on /system needing libraries installed on
        // /system:
        //
        // Pre-installed:
        //   (inactive) package bar version 1 using library version X
        //   package baz version 1 using library version X
        //   (inactive) package foo version 1 using library version X
        //   package sharedlibs version 1 exporting library version X
        //
        // Updated packages (installed on /data/apex/active):
        //   package bar version 2 using library version Y
        //   package foo version 2 using library version Y
        //   package sharedlibs version 2 exporting library version Y

        String baz_apex =
                getTestApex(ApexName.BAZ, ApexType.STRIPPED, ApexVersion.ONE, SharedLibsVersion.X);
        mPreparer.pushResourceFile(baz_apex, "/system/apex/" + baz_apex);
        mPreparer.reboot();

        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.foo/bin/foo_test");
        assertThat(runAsResult).isEqualTo("FOO_VERSION_2 SHARED_LIB_VERSION_Y");
        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.bar/bin/bar_test");
        assertThat(runAsResult).isEqualTo("BAR_VERSION_2 SHARED_LIB_VERSION_Y");
        runAsResult = getDevice().executeShellCommand(
            "/apex/com.android.apex.test.baz/bin/baz_test");
        assertThat(runAsResult).isEqualTo("BAZ_VERSION_1 SHARED_LIB_VERSION_X");
    }
}
