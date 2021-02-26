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

package com.android.tests.apex.host;

import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.cts.install.lib.host.InstallUtilsHost;
import android.platform.test.annotations.LargeTest;

import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.CommandStatus;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;

/**
 * Test for platform support for Apex Compression feature
 */
@RunWith(DeviceJUnit4ClassRunner.class)
public class ApexCompressionTests extends BaseHostJUnit4Test {
    private static final String COMPRESSED_APEX_PACKAGE_NAME = "com.android.apex.compressed";

    private final InstallUtilsHost mHostUtils = new InstallUtilsHost(this);

    private boolean mWasAdbRoot = false;

    @Before
    public void setUp() throws Exception {
        mWasAdbRoot = getDevice().isAdbRoot();
        if (!mWasAdbRoot) {
            assumeTrue("Requires root", getDevice().enableAdbRoot());
        }
        deleteFiles("/system/apex/" + COMPRESSED_APEX_PACKAGE_NAME + "*apex",
                "/data/apex/active/" + COMPRESSED_APEX_PACKAGE_NAME + "*apex");
    }

    @After
    public void tearDown() throws Exception {
        if (!mWasAdbRoot) {
            getDevice().disableAdbRoot();
        }
        deleteFiles("/system/apex/" + COMPRESSED_APEX_PACKAGE_NAME + "*apex",
                "/data/apex/active/" + COMPRESSED_APEX_PACKAGE_NAME + "*apex");
    }

    /**
     * Runs the given phase of a test by calling into the device.
     * Throws an exception if the test phase fails.
     * <p>
     * For example, <code>runPhase("testApkOnlyEnableRollback");</code>
     */
    private void runPhase(String phase) throws Exception {
        assertTrue(runDeviceTests("com.android.tests.apex.app",
                "com.android.tests.apex.app.ApexCompressionTests",
                phase));
    }

    /**
     * Deletes files and reboots the device if necessary.
     * @param files the paths of files which might contain wildcards
     */
    private void deleteFiles(String... files) throws Exception {
        boolean found = false;
        for (String file : files) {
            CommandResult result = getDevice().executeShellV2Command("ls " + file);
            if (result.getStatus() == CommandStatus.SUCCESS) {
                found = true;
                break;
            }
        }

        if (found) {
            getDevice().remountSystemWritable();
            for (String file : files) {
                getDevice().executeShellCommand("rm -rf " + file);
            }
            getDevice().reboot();
        }
    }

    private void pushTestApex() throws Exception {
        final String fileName = COMPRESSED_APEX_PACKAGE_NAME + ".v1.capex";
        final File apex = mHostUtils.getTestFile(fileName);
        getDevice().remountSystemWritable();
        assertTrue(getDevice().pushFile(apex, "/system/apex/" + fileName));
        getDevice().reboot();
    }


    @Test
    @LargeTest
    public void testCompressedApexCanBeQueried() throws Exception {
        pushTestApex();
        runPhase("testCompressedApexCanBeQueried");
    }

}
