// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
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
 *
 * The rows are the same cards as [ToggleRow] and [SegmentedRow]. The text settings are ordinary
 * text fields in those cards, not LibraryKeyboard rows: storage addresses and keys are long, and
 * people copy them from a password manager or the provider's console, so selection, copy and paste
 * matter more here than anywhere else in the settings.
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
            SyncTextRow(
                controllerId = "memcardSync.remote",
                label = str("memcardSync.remote"),
                description = str("memcardSync.remote.desc"),
                value = values.remoteUri,
                keyboardType = KeyboardType.Uri,
            ) {
                MemcardSyncSettings.setRemoteUri(context, it)
                restart()
            }
            SettingsDivider()
            SyncTextRow(
                controllerId = "memcardSync.accessKeyId",
                label = str("memcardSync.accessKeyId"),
                description = str("memcardSync.accessKeyId.desc"),
                value = values.accessKeyId,
                keyboardType = KeyboardType.Ascii,
            ) {
                MemcardSyncSettings.setAccessKeyId(context, it)
                restart()
            }
            // The stored secret is never read back into the field: it starts empty, says whether a
            // secret is saved, and whatever is typed or pasted replaces it. Left empty, it keeps it.
            SettingsDivider()
            SyncTextRow(
                controllerId = "memcardSync.secret",
                label = str("memcardSync.secret"),
                description = str("memcardSync.secret.desc"),
                value = "",
                placeholder = if (values.hasSecret) str("memcardSync.secret.saved") else null,
                keyboardType = KeyboardType.Password,
                secret = true,
            ) {
                if (it.isNotEmpty()) {
                    MemcardSyncSettings.setSecret(context, it)
                    restart()
                }
            }
            SettingsDivider()
            SyncTextRow(
                controllerId = "memcardSync.deviceName",
                label = str("memcardSync.deviceName"),
                description = str("memcardSync.deviceName.desc"),
                value = values.deviceName,
                keyboardType = KeyboardType.Text,
                capitalization = KeyboardCapitalization.Words,
            ) {
                MemcardSyncSettings.setDeviceName(context, it)
                restart()
            }
            val syncingText = str("memcardSync.status.syncing")
            val statusLine = when {
                starting -> str("memcardSync.status.starting")
                error != null -> error!!
                !running -> str("memcardSync.status.incomplete")
                status.isNotBlank() -> status
                else -> str("memcardSync.status.running")
            }
            SettingsDivider()
            SyncStatusRow(
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

/** The card every row of this section sits in: the same fill, border and shape as [ToggleRow]. */
@Composable
private fun SyncCard(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    val tint = rowTint(null)
    Surface(
        // The margin goes outside [modifier], so a controller's selection ring hugs the card.
        modifier = Modifier.fillMaxWidth().padding(vertical = 5.dp).then(modifier),
        shape = RoundedCornerShape(22.dp),
        color = tint.container,
        border = androidx.compose.foundation.BorderStroke(1.dp, tint.border),
    ) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)) { content() }
    }
}

@Composable
private fun RowLabel(label: String, description: String?) {
    Text(label, color = MaterialTheme.colorScheme.onSurface, fontSize = 18.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold)
    if (description != null) {
        Spacer(Modifier.height(3.dp))
        Text(description, color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 14.sp, lineHeight = 19.sp)
    }
}

/**
 * A text setting: label, description, and an ordinary text field. What is typed is kept in the
 * field and handed to [onCommit] (trimmed) when editing ends: the keyboard's Done, the field losing
 * focus, or the row leaving the screen. Not on every keystroke, since each commit restarts sync.
 *
 * On a controller, A on the row puts the cursor in the field and raises the keyboard; moving the
 * selection off the row ends editing. [secret] hides the text, with a Show / Hide switch, and
 * Android then offers paste but not copy, as for any password field.
 */
@Composable
private fun SyncTextRow(
    controllerId: String,
    label: String,
    description: String,
    value: String,
    placeholder: String? = null,
    keyboardType: KeyboardType,
    capitalization: KeyboardCapitalization = KeyboardCapitalization.None,
    secret: Boolean = false,
    onCommit: (String) -> Unit,
) {
    var text by remember(value) { mutableStateOf(value) }
    var editing by remember { mutableStateOf(false) }
    var revealed by remember { mutableStateOf(false) }
    val focusRequester = remember { FocusRequester() }
    val focusManager = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current

    val latestText by rememberUpdatedState(text)
    val latestValue by rememberUpdatedState(value)
    val latestCommit by rememberUpdatedState(onCommit)
    fun commit() {
        val trimmed = latestText.trim()
        if (trimmed != latestValue) latestCommit(trimmed)
        // The secret field never holds what was saved; empty it again once it is stored.
        if (secret) text = ""
    }
    // Leaving the screen mid-edit (Back, another tab) still saves what was typed.
    DisposableEffect(Unit) {
        onDispose { if (latestText.trim() != latestValue) latestCommit(latestText.trim()) }
    }

    // A pad user moving the selection off the row is done with it.
    val selected = SettingsControllerNav.isSelected(controllerId)
    var wasSelected by remember { mutableStateOf(false) }
    LaunchedEffect(selected) {
        if (wasSelected && !selected && editing) focusManager.clearFocus()
        wasSelected = selected
    }

    SyncCard(
        Modifier.controllerFocusable(
            controllerId,
            shape = RoundedCornerShape(22.dp),
            // While the field has the cursor, Enter belongs to it, not to the row.
            onConfirm = if (editing) null else {
                {
                    focusRequester.requestFocus()
                    keyboard?.show()
                }
            },
        ),
    ) {
        RowLabel(label, description)
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            singleLine = true,
            placeholder = placeholder?.let { { Text(it) } },
            visualTransformation = if (secret && !revealed) PasswordVisualTransformation() else VisualTransformation.None,
            trailingIcon = if (secret) {
                {
                    TextButton(onClick = { revealed = !revealed }) {
                        Text(str(if (revealed) "memcardSync.hide" else "memcardSync.show"))
                    }
                }
            } else null,
            keyboardOptions = KeyboardOptions(
                capitalization = capitalization,
                autoCorrectEnabled = false,
                keyboardType = keyboardType,
                imeAction = ImeAction.Done,
            ),
            keyboardActions = KeyboardActions(onDone = { focusManager.clearFocus() }),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier
                .fillMaxWidth()
                .focusRequester(focusRequester)
                .onFocusChanged {
                    if (editing && !it.isFocused) commit()
                    editing = it.isFocused
                },
        )
    }
}

/** The status line, with Sync now beside it while sync is running. */
@Composable
private fun SyncStatusRow(
    label: String,
    value: String,
    action: String?,
    onAction: () -> Unit,
) {
    SyncCard {
        Row(
            Modifier.fillMaxWidth().defaultMinSize(minHeight = 40.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) { RowLabel(label, value) }
            if (action != null) {
                Spacer(Modifier.width(12.dp))
                Button(
                    onClick = onAction,
                    shape = RoundedCornerShape(18.dp),
                    modifier = Modifier.controllerFocusable(
                        "memcardSync.syncNow", RoundedCornerShape(18.dp), onConfirm = onAction,
                    ),
                ) { Text(action) }
            }
        }
    }
}
