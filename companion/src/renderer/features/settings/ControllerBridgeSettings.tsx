import { useEffect, useMemo, useState } from 'react';
import psHomeGlyphUrl from '@product-assets/glyphs/ps5-buttons-outline-white/svg/Home.svg';
import triangleGlyphUrl from '@product-assets/glyphs/ps5-buttons-outline-white/svg/Triangle.svg';
import dpadUpGlyphUrl from '@product-assets/glyphs/ps5-buttons-outline-white/svg/D-Pad Up.svg';
import dpadDownGlyphUrl from '@product-assets/glyphs/ps5-buttons-outline-white/svg/D-Pad Down.svg';
import { CustomSelect } from '../../components/CustomSelect';
import { ToggleRow } from './SettingsToggleRow';
import {
  SearchableSetting,
  SettingsBridgeNotice,
  SettingsGroupLabel,
  settingsUnsupportedMessage,
  type SettingsCategoryProps
} from './SettingsCategoryContentSupport';

const GLYPH_LABELS = {
  home: 'PlayStation Home',
  triangle: 'Triangle',
  dpadUp: 'D-pad Up',
  dpadDown: 'D-pad Down'
} as const;

export const IDLE_DISCONNECT_TIMEOUT_OPTIONS: Array<[string, number]> = [
  ['5 min', 5],
  ['15 min', 15],
  ['30 min', 30]
];

function ShortcutGlyphs({ label, glyphs }: {
  readonly label: string;
  readonly glyphs: ReadonlyArray<ReadonlyArray<readonly [url: string, name: string]>>;
}) {
  return (
    <span className="shortcut-glyph-row" aria-label={label}>
      {glyphs.map((group, index) => (
        <span key={group.map(([, name]) => name).join('-')} className="shortcut-glyph-pair">
          {index > 0 ? <span className="shortcut-plus" aria-hidden="true">+</span> : null}
          {group.map(([url, name]) => (
            <span key={name} className="shortcut-glyph-key"><img src={url} alt={name} /></span>
          ))}
        </span>
      ))}
    </span>
  );
}


/**
 * "Auto Switch Audio on Jack": a companion-wide setting (it drives the Windows
 * default output, not bridge flash), so it stays usable without a bridge. The
 * fallback dropdown only appears when the choice is ambiguous (2+ real
 * outputs). With exactly one other output the companion uses it automatically.
 */
function HeadsetAudioAutoSwitchRow({ model, actions, active }: Pick<SettingsCategoryProps, 'model' | 'actions' | 'active'>) {
  const { settings } = model;
  const [renderEndpoints, setRenderEndpoints] = useState<Array<{ name: string; isBridge: boolean }>>([]);

  // The set of active Windows render endpoints changes as devices come and go,
  // so re-read it whenever the Settings page comes on screen.
  useEffect(() => {
    if (!active) return;
    let cancelled = false;
    actions.listRenderEndpoints().then(
      (endpoints) => { if (!cancelled) setRenderEndpoints(endpoints); },
      () => { if (!cancelled) setRenderEndpoints([]); }
    );
    return () => {
      cancelled = true;
    };
  }, [active]); // eslint-disable-line react-hooks/exhaustive-deps -- actions is rebuilt every render; listing is stateless

  const outputChoices = useMemo(
    () => renderEndpoints.filter((endpoint) => !endpoint.isBridge).map((endpoint) => endpoint.name),
    [renderEndpoints]
  );
  const fallbackDevice = settings.headsetAudioFallbackDevice;
  const savedDeviceAbsent = fallbackDevice !== '' && !outputChoices.includes(fallbackDevice);
  const showFallbackSelect = outputChoices.length >= 2 || savedDeviceAbsent;
  const fallbackOptions = useMemo<Array<[string, string]>>(() => {
    const options: Array<[string, string]> = [['Auto (only other output)', '']];
    for (const name of outputChoices) options.push([name, name]);
    // Keep a saved device selectable while it is off, so the choice survives a power cycle.
    if (savedDeviceAbsent) options.push([`${fallbackDevice} (not connected)`, fallbackDevice]);
    return options;
  }, [outputChoices, fallbackDevice, savedDeviceAbsent]);

  const toggle = (
    <button
      type="button"
      role="switch"
      aria-labelledby="settings-headset-audio-label"
      aria-checked={settings.headsetAudioAutoSwitchEnabled}
      className={`switch ${settings.headsetAudioAutoSwitchEnabled ? 'on' : ''}`}
      disabled={model.busy}
      onClick={actions.toggleHeadsetAudioAutoSwitch}
    >
      <span />
    </button>
  );

  return (
    <div className="settings-menu-row">
      <div className="settings-menu-copy">
        <strong id="settings-headset-audio-label">Auto Switch Audio on Jack</strong>
        <span>Switches Windows output to the controller when a headset is plugged in, and back when it is removed</span>
      </div>
      {showFallbackSelect ? (
        <div className="settings-menu-controls headset-audio-controls">
          <CustomSelect
            value={fallbackDevice}
            options={fallbackOptions}
            className="headset-audio-fallback-select"
            showSelectedCheck={false}
            floatingMenu
            floatingMenuAlign="end"
            floatingMenuMinWidth={320}
            ariaLabel="Return audio to this device when the headset is unplugged"
            disabled={model.busy || !settings.headsetAudioAutoSwitchEnabled}
            onChange={actions.setHeadsetAudioFallbackDevice}
          />
          {toggle}
        </div>
      ) : toggle}
    </div>
  );
}
export function ConnectionPowerSettings({ model, actions, active, matchingSettingIds }: SettingsCategoryProps) {
  const { settings } = model;
  return <>
    <h3 className="settings-menu-section-label">Connection &amp; Power</h3>
    <SettingsBridgeNotice model={model} ids={['idle-disconnect', 'pc-sleep-disconnect']} matchingSettingIds={matchingSettingIds} />
    <SearchableSetting id="idle-disconnect" matchingSettingIds={matchingSettingIds}>
      <div className="settings-menu-row">
        <div className="settings-menu-copy">
          <strong id="settings-idle-disconnect-label">Idle Disconnect</strong>
          <span>Disconnect the controller after it sits unused</span>
        </div>
        <div className="settings-menu-controls">
          <CustomSelect
            value={settings.idleDisconnectTimeoutMinutes}
            options={IDLE_DISCONNECT_TIMEOUT_OPTIONS}
            className="settings-timeout-select"
            floatingMenu
            floatingMenuAlign="end"
            showSelectedCheck={false}
            ariaLabel="Idle disconnect timeout"
            disabled={!model.connected || !settings.idleDisconnectEnabled || model.busy}
            onChange={actions.setIdleDisconnectTimeout}
          />
          <button
            type="button"
            role="switch"
            aria-labelledby="settings-idle-disconnect-label"
            aria-checked={settings.idleDisconnectEnabled}
            className={`switch ${settings.idleDisconnectEnabled ? 'on' : ''}`}
            disabled={!model.connected || model.busy}
            onClick={actions.toggleIdleDisconnect}
          >
            <span />
          </button>
        </div>
      </div>
    </SearchableSetting>
    <SearchableSetting id="pc-sleep-disconnect" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="PC Sleep Disconnect"
        description="Disconnect the controller when the PC sleeps"
        checked={settings.usbSuspendDisconnectEnabled}
        disabled={!model.connected || !model.support.usbSuspendDisconnect || model.busy}
        unavailableReason={settingsUnsupportedMessage(model, model.support.usbSuspendDisconnect)}
        onToggle={actions.toggleUsbSuspendDisconnect}
      />
    </SearchableSetting>
    <SearchableSetting id="headset-audio-auto-switch" matchingSettingIds={matchingSettingIds}>
      <HeadsetAudioAutoSwitchRow model={model} actions={actions} active={active} />
    </SearchableSetting>
    <SettingsGroupLabel label="Power" ids={['controller-power-saving']} matchingSettingIds={matchingSettingIds} />
    <SearchableSetting id="controller-power-saving" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Controller Power Saving"
        description="Caps haptics, triggers, and lightbar brightness at 60% while headphones are plugged in"
        checked={settings.controllerPowerSavingEnabled}
        disabled={model.busy}
        highlighted={model.focusTarget === 'controller-power-saving'}
        onToggle={actions.toggleControllerPowerSaving}
      />
    </SearchableSetting>
  </>;
}

export function LightsSettings({ model, actions, matchingSettingIds }: SettingsCategoryProps) {
  const { settings } = model;
  return <>
    <h3 className="settings-menu-section-label">Lights &amp; Indicators</h3>
    <SettingsBridgeNotice model={model} ids={['pico-led', 'player-slot-led', 'lightbar-restore']} matchingSettingIds={matchingSettingIds} />
    <SearchableSetting id="pico-led" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Bridge LED"
        description="Light the Pico status LED"
        checked={settings.ledEnabled}
        disabled={!model.connected || model.busy}
        onToggle={actions.toggleLed}
      />
    </SearchableSetting>
    <SearchableSetting id="player-slot-led" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Player LEDs"
        description="Show the controller player indicator lights"
        checked={settings.playerLedEnabled}
        disabled={!model.connected || model.busy}
        onToggle={actions.togglePlayerLed}
      />
    </SearchableSetting>
    <SettingsGroupLabel label="Light Bar" ids={['lightbar-restore']} matchingSettingIds={matchingSettingIds} />
    <SearchableSetting id="lightbar-restore" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Automatic Restore"
        description="Reapply the saved color after games clear it"
        checked={settings.lightbarRestoreEnabled}
        disabled={!model.connected || model.busy}
        onToggle={actions.toggleLightbarRestore}
      />
    </SearchableSetting>
  </>;
}

export function ShortcutSettings({ model, actions, matchingSettingIds }: SettingsCategoryProps) {
  const { settings } = model;
  return <>
    <h3 className="settings-menu-section-label">Shortcuts</h3>
    <SettingsBridgeNotice
      model={model}
      ids={['edge-profile-switching', 'sleep-shortcut', 'volume-shortcut']}
      matchingSettingIds={matchingSettingIds}
    />
    <SearchableSetting id="sleep-shortcut" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Sleep Shortcut"
        description="Put the controller to sleep from the controller"
        tooltip={(
          <span className="settings-shortcut-tooltip-copy">
            <span>Put controller to sleep with</span>
            <ShortcutGlyphs
              label="PlayStation Home and Triangle"
              glyphs={[[[psHomeGlyphUrl, GLYPH_LABELS.home]], [[triangleGlyphUrl, GLYPH_LABELS.triangle]]]}
            />
          </span>
        )}
        checked={settings.sleepKeybindEnabled}
        disabled={!model.connected || !model.support.sleepController || model.busy}
        unavailableReason={settingsUnsupportedMessage(model, model.support.sleepController)}
        highlighted={model.focusTarget === 'sleep-shortcut' || model.focusTarget === 'shortcuts'}
        onToggle={actions.toggleSleepShortcut}
      />
    </SearchableSetting>
    <SearchableSetting id="volume-shortcut" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Volume Shortcut"
        description="Change the controller speaker volume from the controller"
        tooltip={(
          <span className="settings-shortcut-tooltip-copy">
            <span>Controller volume up/down with</span>
            <ShortcutGlyphs
              label="PlayStation Home and D-pad Up or D-pad Down"
              glyphs={[[[psHomeGlyphUrl, GLYPH_LABELS.home]], [[dpadUpGlyphUrl, GLYPH_LABELS.dpadUp], [dpadDownGlyphUrl, GLYPH_LABELS.dpadDown]]]}
            />
          </span>
        )}
        checked={settings.speakerVolumeShortcutEnabled}
        disabled={!model.connected || model.busy}
        highlighted={model.focusTarget === 'volume-shortcut' || model.focusTarget === 'shortcuts'}
        onToggle={actions.toggleVolumeShortcut}
      />
    </SearchableSetting>
    <SearchableSetting id="edge-profile-switching" matchingSettingIds={matchingSettingIds}>
      <ToggleRow
        label="Block Edge Profile Switching"
        description="Reserve LFN/RFN + face buttons for chords when a DualSense Edge connects to this bridge"
        checked={settings.edgeProfileSwitchingBlocked}
        disabled={!model.connected || model.busy}
        onToggle={actions.toggleEdgeProfileSwitching}
      />
    </SearchableSetting>
  </>;
}
