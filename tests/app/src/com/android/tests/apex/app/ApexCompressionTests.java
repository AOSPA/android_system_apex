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

package com.android.tests.apex.app;

import static com.google.common.truth.Truth.assertThat;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;

import androidx.test.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public class ApexCompressionTests {
    private static final String COMPRESSED_APEX_PACKAGE_NAME = "com.android.apex.compressed";

    @Test
    public void testCompressedApexCanBeQueried() throws Exception {
        Context context = InstrumentationRegistry.getContext();
        PackageManager pm = context.getPackageManager();
        PackageInfo pi = pm.getPackageInfo(COMPRESSED_APEX_PACKAGE_NAME,
                PackageManager.MATCH_APEX | PackageManager.MATCH_FACTORY_ONLY);
        assertThat(pi).isNotNull();
        assertThat(pi.isApex).isTrue();
        assertThat(pi.packageName).isEqualTo(COMPRESSED_APEX_PACKAGE_NAME);
        assertThat(pi.getLongVersionCode()).isEqualTo(1);
    }
}
