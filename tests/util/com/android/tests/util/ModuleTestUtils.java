/*
 * Copyright (C) 2019 The Android Open Source Project
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

package com.android.tests.util;

import com.android.tradefed.build.BuildInfoKey.BuildInfoFileKey;
import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.log.LogUtil.CLog;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.FileUtil;
import com.android.tradefed.util.SystemUtil.EnvVariable;

import junit.framework.Assert;

import java.io.File;
import java.io.IOException;
import java.time.Duration;
import java.util.regex.Pattern;
import java.util.stream.Stream;

public class ModuleTestUtils {
    private static final Duration WAIT_FOR_SESSION_READY_TTL = Duration.ofSeconds(10);
    private static final Duration SLEEP_FOR = Duration.ofMillis(200);

    protected final Pattern mIsSessionReadyPattern =
            Pattern.compile("(isReady = true)|(isStagedSessionReady = true)");
    protected final Pattern mIsSessionAppliedPattern =
            Pattern.compile("(isApplied = true)|(isStagedSessionApplied = true)");

    private BaseHostJUnit4Test mTest;

    public ModuleTestUtils(BaseHostJUnit4Test test) {
        mTest = test;
    }

    /**
     * Get the test file.
     *
     * @param testFileName name of the file
     */
    public File getTestFile(String testFileName) throws IOException {
        File testFile = null;

        String testcasesPath = System.getenv(EnvVariable.ANDROID_HOST_OUT_TESTCASES.toString());
        if (testcasesPath != null) {
            testFile = searchTestFile(new File(testcasesPath), testFileName);
        }
        if (testFile != null) {
            return testFile;
        }

        File hostLinkedDir = mTest.getBuild().getFile(BuildInfoFileKey.HOST_LINKED_DIR);
        if (hostLinkedDir != null) {
            testFile = searchTestFile(hostLinkedDir, testFileName);
        }
        if (testFile != null) {
            return testFile;
        }

        // Find the file in the buildinfo.
        File buildInfoFile = mTest.getBuild().getFile(testFileName);
        if (buildInfoFile != null) {
            return buildInfoFile;
        }

        throw new IOException("Cannot find " + testFileName);
    }

    /**
     * Searches the file with the given name under the given directory, returns null if not found.
     */
    private File searchTestFile(File baseSearchFile, String testFileName) {
        if (baseSearchFile != null && baseSearchFile.isDirectory()) {
            File testFile = FileUtil.findFile(baseSearchFile, testFileName);
            if (testFile != null && testFile.isFile()) {
                return testFile;
            }
        }
        return null;
    }

    public void waitForStagedSessionReady() throws DeviceNotAvailableException {
        // TODO: implement wait for session ready logic inside PackageManagerShellCommand instead.
        boolean sessionReady = false;
        Duration spentWaiting = Duration.ZERO;
        while (spentWaiting.compareTo(WAIT_FOR_SESSION_READY_TTL) < 0) {
            CommandResult res = mTest.getDevice().executeShellV2Command("pm get-stagedsessions");
            Assert.assertEquals("", res.getStderr());
            sessionReady = Stream.of(res.getStdout().split("\n")).anyMatch(this::isReadyNotApplied);
            if (sessionReady) {
                CLog.i("Done waiting after " + spentWaiting);
                break;
            }
            try {
                Thread.sleep(SLEEP_FOR.toMillis());
                spentWaiting = spentWaiting.plus(SLEEP_FOR);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new RuntimeException(e);
            }
        }
        Assert.assertTrue("Staged session wasn't ready in " + WAIT_FOR_SESSION_READY_TTL,
                sessionReady);
    }

    private boolean isReadyNotApplied(String sessionInfo) {
        boolean isReady = mIsSessionReadyPattern.matcher(sessionInfo).find();
        boolean isApplied = mIsSessionAppliedPattern.matcher(sessionInfo).find();
        return isReady && !isApplied;
    }
}
