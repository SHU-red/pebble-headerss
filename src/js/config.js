/**
 * HeadeRSS — Clay configuration page definition.
 *
 * Built with @rebble/clay. Watch-bound messageKeys (AccentColor, DarkMode,
 * TouchEnabled, HighlightWords) match entries of the "messageKeys" array in
 * package.json so Clay sends their values to the watch on save. The reading
 * options (auto-mark mode, unread-only, smart-surface toggles) live on the
 * WATCH in the sub-menu and are not part of this page. The connection
 * fields (ServerUrl, User, ApiPass) are phone-side only: Clay persists them
 * in 'clay-settings' for prefill and the JS keeps a working copy in
 * localStorage; they never round-trip to the watch.
 */

module.exports = [
  {
    'type': 'heading',
    'defaultValue': 'HeadeRSS'
  },
  {
    'type': 'section',
    'items': [
      {
        'type': 'heading',
        'defaultValue': 'Connection'
      },
      {
        'type': 'input',
        'messageKey': 'ServerUrl',
        'label': 'FreshRSS URL',
        'attributes': {
          'placeholder': 'http://192.168.178.55:8080'
        }
      },
      {
        'type': 'input',
        'messageKey': 'User',
        'label': 'Username',
        'attributes': {}
      },
      {
        'type': 'input',
        'messageKey': 'ApiPass',
        'label': 'API password',
        'attributes': {
          'type': 'password'
        },
        'description': 'FreshRSS profile → API password, NOT the login password'
      }
    ]
  },
  {
    'type': 'section',
    'items': [
      {
        'type': 'heading',
        'defaultValue': 'Appearance'
      },
      {
        'type': 'color',
        'messageKey': 'AccentColor',
        'label': 'Accent color',
        'defaultValue': 21930  // 0x0055AA
      },
      {
        'type': 'select',
        'messageKey': 'DarkMode',
        'label': 'Appearance',
        'options': [
          { 'label': 'Dark', 'value': 1 },
          { 'label': 'Light', 'value': 0 }
        ],
        'defaultValue': 1
      },
      {
        'type': 'toggle',
        'messageKey': 'TouchEnabled',
        'label': 'Touch',
        'description': 'Off while firmware touch bugs persist (PebbleOS #1865)',
        'defaultValue': false
      },
      {
        'type': 'input',
        'messageKey': 'HighlightWords',
        'id': 'highlightWords',
        'label': 'Highlight words',
        'description': 'Comma-separated words/phrases (max 10) highlighted in articles',
        'attributes': {
          'maxlength': 200
        }
      }
    ]
  },
  {
    'type': 'submit',
    'defaultValue': 'Save Settings'
  }
];
