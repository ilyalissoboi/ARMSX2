// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.str
import com.armsx2.sync.MemcardSyncSettings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Memory card sync (emusync) in Settings > Network. Every change is stored at once and sync is
 * restarted with it on a background thread, since starting checks the storage over the network.
 * A problem starting is shown here; the status line below it follows the running sync.
 */
@Composable
fun MemcardSyncSection() {
    val context = LocalContext.current
    CollapsibleSection(str("memcardSync.title")) {
        if (!MemcardSyncSettings.isAvailable()) {
            HelpText(str("memcardSync.unavailable"))
            return@CollapsibleSection
        }
        var values by remember { mutableStateOf(MemcardSyncSettings.load(context)) }
        var starting by remember { mutableStateOf(false) }
        var error by remember { mutableStateOf<String?>(null) }
        var status by remember { mutableStateOf(MemcardSyncSettings.status()) }
        var running by remember { mutableStateOf(MemcardSyncSettings.isRunning()) }
        val scope = rememberCoroutineScope()

        // The core updates its status from its own thread; a light poll while this is on screen.
        LaunchedEffect(Unit) {
            while (true) {
                status = MemcardSyncSettings.status()
                running = MemcardSyncSettings.isRunning()
                delay(2000)
            }
        }

        fun restart() {
            values = MemcardSyncSettings.load(context)
            starting = true
            scope.launch {
                error = withContext(Dispatchers.IO) { MemcardSyncSettings.start(context) }
                running = MemcardSyncSettings.isRunning()
                status = MemcardSyncSettings.status()
                starting = false
            }
        }

        HelpText(str("memcardSync.help"))
        ToggleRow(
            str("memcardSync.enable"),
            values.enabled,
            description = str("memcardSync.enable.desc"),
        ) {
            MemcardSyncSettings.setEnabled(context, it)
            restart()
        }
        if (values.enabled) {
            SettingsDivider()
            SyncFieldRow(
                controllerId = "memcardSync.remote",
                label = str("memcardSync.remote"),
                value = values.remoteUri,
                description = str("memcardSync.remote.desc"),
            ) {
                MemcardSyncSettings.setRemoteUri(context, it)
                restart()
            }
            SettingsDivider()
            SyncFieldRow(
                controllerId = "memcardSync.accessKeyId",
                label = str("memcardSync.accessKeyId"),
                value = values.accessKeyId,
                description = str("memcardSync.accessKeyId.desc"),
            ) {
                MemcardSyncSettings.setAccessKeyId(context, it)
                restart()
            }
            SettingsDivider()
            SyncFieldRow(
                controllerId = "memcardSync.secret",
                label = str("memcardSync.secret"),
                // Never shown, never prefilled: the editor starts empty and an empty entry keeps it.
                value = if (values.hasSecret) "••••••••" else "",
                editValue = "",
                description = str("memcardSync.secret.desc"),
            ) {
                if (it.isNotBlank()) {
                    MemcardSyncSettings.setSecret(context, it)
                    restart()
                }
            }
            SettingsDivider()
            SyncFieldRow(
                controllerId = "memcardSync.deviceName",
                label = str("memcardSync.deviceName"),
                value = values.deviceName,
                description = str("memcardSync.deviceName.desc"),
            ) {
                MemcardSyncSettings.setDeviceName(context, it)
                restart()
            }
            SettingsDivider()
            val syncingText = str("memcardSync.status.syncing")
            val statusLine = when {
                starting -> str("memcardSync.status.starting")
                error != null -> error!!
                !running -> str("memcardSync.status.incomplete")
                status.isNotBlank() -> status
                else -> str("memcardSync.status.running")
            }
            SyncInfoRow(
                controllerId = "memcardSync.syncNow",
                label = str("memcardSync.status"),
                value = statusLine,
                action = if (running && !starting) str("memcardSync.syncNow") else null,
            ) {
                MemcardSyncSettings.syncNow()
                status = syncingText
            }
            HelpText(str("memcardSync.cards.help"))
        }
        Spacer(Modifier.height(8.dp))
    }
}

/** A text setting, edited with LibraryKeyboard (pad-navigable; no dialog, which would trap a pad). */
@Composable
private fun SyncFieldRow(
    controllerId: String,
    label: String,
    value: String,
    description: String,
    editValue: String = value,
    onChange: (String) -> Unit,
) {
    val edit = { com.armsx2.ui.home.LibraryKeyboard.open(editValue, { onChange(it.trim()) }, label) }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .clickable(onClick = edit)
            .controllerFocusable(controllerId, onConfirm = edit)
            .padding(horizontal = 6.dp, vertical = 8.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.weight(1f))
            Text(
                value.ifEmpty { str("memcardSync.notSet") },
                color = Color(0xFFCCCCCC),
                fontSize = 14.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 12.dp),
            )
        }
        Text(
            description,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 3.dp),
        )
    }
}

/** The status line, with an optional action (Sync now) on confirm. */
@Composable
private fun SyncInfoRow(
    controllerId: String,
    label: String,
    value: String,
    action: String?,
    onAction: () -> Unit,
) {
    val modifier = if (action != null) {
        Modifier.clickable(onClick = onAction).controllerFocusable(controllerId, onConfirm = onAction)
    } else {
        Modifier
    }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(rowAura())
            .then(modifier)
            .padding(horizontal = 6.dp, vertical = 8.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.weight(1f))
            if (action != null) {
                Text(action, color = MaterialTheme.colorScheme.primary, fontSize = 15.sp, fontWeight = FontWeight.Bold)
            }
        }
        Text(
            value,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 13.sp,
            modifier = Modifier.padding(top = 3.dp),
        )
    }
}
