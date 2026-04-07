import AsyncStorage from '@react-native-async-storage/async-storage';
import { StatusBar } from 'expo-status-bar';
import {
  Image,
  KeyboardAvoidingView,
  Platform,
  Pressable,
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { startTransition, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';

type TabId = 'listen' | 'publish';

type BootstrapPayload = {
  scheme: string;
  port: number;
  path: string;
  addresses: string[];
};

type PersistedState = {
  address_input: string;
  topics_input: string;
  publish_topic: string;
  publish_draft: string;
  publish_history: string[];
  bootstrap_payload: BootstrapPayload | null;
};

type ListenConfig = {
  base_address: string;
  path: string;
  topics: string[];
};

type StatusTone = 'neutral' | 'success' | 'warning' | 'danger';

type StatusInfo = {
  text: string;
  tone: StatusTone;
};

const kStorageKey = 'mads-websockets-react-client/state/v1';
const kWebSocketProtocol = 'mads-websockets';
const kMadsLogo = require('./assets/mads_logo_white.png');
const kDefaultBootstrap: BootstrapPayload = {
  scheme: 'ws',
  port: 9002,
  path: '/mads',
  addresses: [],
};
const kDefaultPersistedState: PersistedState = {
  address_input: '',
  topics_input: '',
  publish_topic: '',
  publish_draft: '{\n  \n}',
  publish_history: [],
  bootstrap_payload: null,
};

export default function App() {
  const [active_tab, set_active_tab] = useState<TabId>('listen');
  const [hydrated, set_hydrated] = useState(false);

  const [address_input, set_address_input] = useState(kDefaultPersistedState.address_input);
  const [topics_input, set_topics_input] = useState(kDefaultPersistedState.topics_input);
  const [publish_topic, set_publish_topic] = useState(kDefaultPersistedState.publish_topic);
  const [publish_draft, set_publish_draft] = useState(kDefaultPersistedState.publish_draft);
  const [publish_history, set_publish_history] = useState<string[]>(
    kDefaultPersistedState.publish_history
  );
  const [bootstrap_payload, set_bootstrap_payload] = useState<BootstrapPayload | null>(
    kDefaultPersistedState.bootstrap_payload
  );

  const [listen_config, set_listen_config] = useState<ListenConfig | null>(null);
  const [listen_messages, set_listen_messages] = useState<Record<string, unknown>>({});
  const [listen_status, set_listen_status] = useState<StatusInfo>({
    text: 'Choose topics and subscribe to start listening.',
    tone: 'neutral',
  });
  const [publish_status, set_publish_status] = useState<StatusInfo>({
    text: 'Pick a topic to keep a publisher socket ready.',
    tone: 'neutral',
  });
  const [history_menu_open, set_history_menu_open] = useState(false);
  const [expand_all, set_expand_all] = useState(false);

  const listen_sockets_ref = useRef<WebSocket[]>([]);
  const publish_socket_ref = useRef<WebSocket | null>(null);
  const listen_session_ref = useRef(0);

  useEffect(() => {
    let cancelled = false;
    AsyncStorage.getItem(kStorageKey)
      .then((stored_value) => {
        if (cancelled || stored_value == null) {
          return;
        }
        const parsed = JSON.parse(stored_value) as Partial<PersistedState>;
        set_address_input(parsed.address_input ?? kDefaultPersistedState.address_input);
        set_topics_input(parsed.topics_input ?? kDefaultPersistedState.topics_input);
        set_publish_topic(parsed.publish_topic ?? kDefaultPersistedState.publish_topic);
        set_publish_draft(parsed.publish_draft ?? kDefaultPersistedState.publish_draft);
        set_publish_history(parsed.publish_history ?? kDefaultPersistedState.publish_history);
        set_bootstrap_payload(
          sanitize_bootstrap_payload(parsed.bootstrap_payload) ?? kDefaultPersistedState.bootstrap_payload
        );
      })
      .catch((error: unknown) => {
        console.warn('Failed to restore client state', error);
      })
      .finally(() => {
        if (!cancelled) {
          set_hydrated(true);
        }
      });

    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    if (!hydrated) {
      return;
    }
    const state_to_store: PersistedState = {
      address_input,
      topics_input,
      publish_topic,
      publish_draft,
      publish_history,
      bootstrap_payload,
    };
    AsyncStorage.setItem(kStorageKey, JSON.stringify(state_to_store)).catch((error: unknown) => {
      console.warn('Failed to persist client state', error);
    });
  }, [
    address_input,
    bootstrap_payload,
    hydrated,
    publish_draft,
    publish_history,
    publish_topic,
    topics_input,
  ]);

  useEffect(() => {
    if (!hydrated || Platform.OS !== 'web' || typeof window === 'undefined') {
      return;
    }

    let cancelled = false;
    fetch(`${window.location.origin}/bootstrap.json`)
      .then(async (response) => {
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }
        return response.text();
      })
      .then((payload) => {
        if (cancelled) {
          return;
        }
        const parsed = parse_bootstrap_payload(payload);
        if (parsed == null) {
          throw new Error('Invalid bootstrap payload');
        }
        set_bootstrap_payload(parsed);
        set_address_input((current) => {
          const replacement = `${window.location.hostname}:${parsed.port}`;
          if (current.trim().length === 0) {
            return replacement;
          }

          const existing = parse_browser_address(current);
          if (existing == null) {
            return current;
          }

          if (
            existing.hostname === window.location.hostname &&
            (existing.port === window.location.port ||
              existing.protocol === 'http:' ||
              existing.protocol === 'https:')
          ) {
            return replacement;
          }

          return current;
        });
      })
      .catch((error: unknown) => {
        console.warn('Failed to load browser bootstrap', error);
      });

    return () => {
      cancelled = true;
    };
  }, [hydrated]);

  useEffect(() => {
    if (active_tab !== 'listen' || listen_config == null) {
      close_listen_sockets(listen_sockets_ref);
      if (listen_config != null) {
        set_listen_status({
          text: 'Listening paused. Open the Listen tab to resume updates.',
          tone: 'warning',
        });
      }
      return;
    }

    close_listen_sockets(listen_sockets_ref);
    set_listen_status({
      text: `Connecting to ${listen_config.topics.join(', ')}...`,
      tone: 'neutral',
    });

    const session_id = ++listen_session_ref.current;
    let opened_connections = 0;
    listen_sockets_ref.current = listen_config.topics.map((topic) => {
      const socket = new WebSocket(
        build_topic_url(listen_config.base_address, listen_config.path, topic),
        kWebSocketProtocol
      );
      socket.onopen = () => {
        if (listen_session_ref.current !== session_id) {
          return;
        }
        opened_connections += 1;
        set_listen_status({
          text: `Listening on ${opened_connections}/${listen_config.topics.length} connection(s).`,
          tone: 'success',
        });
      };
      socket.onmessage = (event) => {
        if (listen_session_ref.current !== session_id) {
          return;
        }
        try {
          const parsed = JSON.parse(event.data as string) as unknown;
          const incoming = normalize_incoming_message(topic, parsed);
          startTransition(() => {
            set_listen_messages((current) => ({
              ...current,
              [incoming.topic]: incoming.message,
            }));
          });
        } catch (error: unknown) {
          set_listen_status({
            text: `Received invalid JSON on ${topic}: ${stringify_error(error)}`,
            tone: 'danger',
          });
        }
      };
      socket.onerror = () => {
        if (listen_session_ref.current !== session_id) {
          return;
        }
        set_listen_status({
          text: `Listener connection failed for topic ${topic}.`,
          tone: 'danger',
        });
      };
      socket.onclose = () => {
        if (listen_session_ref.current !== session_id) {
          return;
        }
        if (active_tab === 'listen') {
          set_listen_status({
            text: `Listener for ${topic} closed.`,
            tone: 'warning',
          });
        }
      };
      return socket;
    });

    return () => {
      close_listen_sockets(listen_sockets_ref);
    };
  }, [active_tab, listen_config]);

  const resolved_endpoint = useMemo(
    () => resolve_endpoint(address_input, bootstrap_payload),
    [address_input, bootstrap_payload]
  );

  const publish_json_error = useMemo(() => {
    const trimmed = publish_draft.trim();
    if (trimmed.length === 0) {
      return 'Enter a JSON payload.';
    }
    try {
      JSON.parse(trimmed);
      return null;
    } catch (error: unknown) {
      return stringify_error(error);
    }
  }, [publish_draft]);

  useEffect(() => {
    if (resolved_endpoint == null || publish_topic.trim().length === 0) {
      close_publish_socket(publish_socket_ref);
      set_publish_status({
        text:
          resolved_endpoint == null
            ? 'Waiting for bridge bootstrap to enable publishing.'
            : 'Pick a topic to keep a publisher socket ready.',
        tone: resolved_endpoint == null ? 'warning' : 'neutral',
      });
      return;
    }

    close_publish_socket(publish_socket_ref);
    set_publish_status({
      text: `Connecting publisher to ${publish_topic.trim()}...`,
      tone: 'neutral',
    });

    const socket = new WebSocket(
      build_topic_url(resolved_endpoint.base_address, resolved_endpoint.path, publish_topic.trim()),
      kWebSocketProtocol
    );
    publish_socket_ref.current = socket;

    socket.onopen = () => {
      if (publish_socket_ref.current !== socket) {
        return;
      }
      set_publish_status({
        text: `Publisher ready on ${publish_topic.trim()}.`,
        tone: 'success',
      });
    };
    socket.onerror = () => {
      if (publish_socket_ref.current !== socket) {
        return;
      }
      set_publish_status({
        text: `Publisher connection failed for ${publish_topic.trim()}.`,
        tone: 'danger',
      });
    };
    socket.onclose = () => {
      if (publish_socket_ref.current !== socket) {
        return;
      }
      set_publish_status({
        text: `Publisher disconnected for ${publish_topic.trim()}.`,
        tone: 'warning',
      });
    };

    return () => {
      if (publish_socket_ref.current === socket) {
        close_publish_socket(publish_socket_ref);
      } else {
        socket.close();
      }
    };
  }, [publish_topic, resolved_endpoint]);

  const subscribe_topics = useMemo(() => parse_subscription_topics(topics_input), [topics_input]);
  const can_subscribe = resolved_endpoint != null;
  const can_send =
    publish_json_error == null &&
    publish_topic.trim().length > 0 &&
    publish_socket_ref.current?.readyState === WebSocket.OPEN;

  useEffect(() => {
    if (active_tab !== 'listen' || resolved_endpoint == null || listen_config != null) {
      return;
    }

    set_listen_config({
      base_address: resolved_endpoint.base_address,
      path: resolved_endpoint.path,
      topics: subscribe_topics,
    });
    set_listen_status({
      text: `Subscribing to ${subscribe_topics.join(', ')}...`,
      tone: 'neutral',
    });
  }, [active_tab, listen_config, resolved_endpoint, subscribe_topics]);

  function handle_subscribe_press() {
    if (resolved_endpoint == null) {
      set_listen_status({
        text: 'Waiting for bridge bootstrap before subscribing.',
        tone: 'warning',
      });
      return;
    }
    set_listen_config({
      base_address: resolved_endpoint.base_address,
      path: resolved_endpoint.path,
      topics: subscribe_topics,
    });
    set_listen_status({
      text: `Updating subscriptions to ${subscribe_topics.join(', ')}...`,
      tone: 'neutral',
    });
  }

  function handle_send_press() {
    const socket = publish_socket_ref.current;
    if (socket == null || socket.readyState !== WebSocket.OPEN || publish_json_error != null) {
      return;
    }
    socket.send(publish_draft.trim());
    set_publish_history((current) => {
      const next = [publish_topic.trim(), ...current.filter((value) => value !== publish_topic.trim())];
      return next.slice(0, 12);
    });
    set_publish_status({
      text: `Sent message on ${publish_topic.trim()}.`,
      tone: 'success',
    });
  }

  return (
    <SafeAreaView style={styles.safe_area}>
      <StatusBar style="dark" />
      <KeyboardAvoidingView
        style={styles.shell}
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
      >
        <View style={styles.header}>
          <Image source={kMadsLogo} style={styles.header_logo} resizeMode="contain" />
          <Text style={styles.header_title}>Websocket bridge</Text>
        </View>

        <View style={styles.tab_bar}>
          {([
            ['listen', 'Listen'],
            ['publish', 'Publish'],
          ] as const).map(([tab_id, label]) => (
            <Pressable
              key={tab_id}
              style={[
                styles.tab_button,
                active_tab === tab_id && styles.tab_button_active,
              ]}
              onPress={() => set_active_tab(tab_id)}
            >
              <Text
                style={[
                  styles.tab_button_text,
                  active_tab === tab_id && styles.tab_button_text_active,
                ]}
              >
                {label}
              </Text>
            </Pressable>
          ))}
        </View>

        <ScrollView
          style={styles.scroll}
          contentContainerStyle={styles.scroll_content}
          keyboardShouldPersistTaps="handled"
        >
          {active_tab === 'listen' ? (
            <View style={styles.panel}>
              <Text style={styles.section_title}>Latest messages</Text>
              <Text style={styles.section_body}>
                Update the topic list here. The browser reconnects listener sockets when you subscribe.
              </Text>
              <View style={styles.subscribe_row}>
                <View style={styles.subscribe_input_wrap}>
                  <TextInput
                    value={topics_input}
                    onChangeText={set_topics_input}
                    placeholder="_all or topic_a, topic_b"
                    placeholderTextColor="#9f927e"
                    autoCapitalize="none"
                    autoCorrect={false}
                    style={styles.input}
                  />
                </View>
                <Pressable
                  style={[styles.primary_button, !can_subscribe && styles.button_disabled, styles.subscribe_button]}
                  onPress={handle_subscribe_press}
                  disabled={!can_subscribe}
                >
                  <Text style={styles.primary_button_text}>Subscribe</Text>
                </Pressable>
              </View>
              <View style={styles.info_card}>
                <Text style={styles.info_card_label}>Subscription set</Text>
                <Text style={styles.info_card_value}>{subscribe_topics.join(', ')}</Text>
                <Text style={styles.info_card_hint}>
                  {resolved_endpoint == null
                    ? 'Waiting for bridge bootstrap.'
                    : `${resolved_endpoint.base_address}${resolved_endpoint.path}`}
                </Text>
              </View>
              <StatusPill status={listen_status} />
              <View style={styles.row}>
                <Pressable
                  style={styles.secondary_button}
                  onPress={() => set_expand_all((current) => !current)}
                >
                  <Text style={styles.secondary_button_text}>
                    {expand_all ? 'Collapse all' : 'Expand all'}
                  </Text>
                </Pressable>
                <Pressable
                  style={styles.secondary_button}
                  onPress={() => set_listen_messages({})}
                >
                  <Text style={styles.secondary_button_text}>Clear</Text>
                </Pressable>
              </View>

              {Object.keys(listen_messages).length === 0 ? (
                <Text style={styles.empty_state}>
                  No messages captured yet. Subscribe and keep this tab open to receive updates.
                </Text>
              ) : (
                Object.entries(listen_messages)
                  .sort(([left], [right]) => left.localeCompare(right))
                  .map(([topic, message]) => (
                    <View key={`${topic}-${expand_all ? 'open' : 'closed'}`} style={styles.message_card}>
                      <JsonTreeNode
                        label={topic}
                        value={message}
                        default_expanded={expand_all}
                        level={0}
                      />
                    </View>
                  ))
              )}
            </View>
          ) : null}

          {active_tab === 'publish' ? (
            <View style={styles.panel}>
              <Text style={styles.section_title}>Publisher</Text>
              <StatusPill status={publish_status} />

              <LabeledField label="Topic">
                <TextInput
                  value={publish_topic}
                  onChangeText={set_publish_topic}
                  placeholder="topic_name"
                  placeholderTextColor="#9f927e"
                  autoCapitalize="none"
                  autoCorrect={false}
                  style={styles.input}
                />
              </LabeledField>

              <View style={styles.row}>
                <Pressable
                  style={styles.secondary_button}
                  onPress={() => set_history_menu_open((current) => !current)}
                >
                  <Text style={styles.secondary_button_text}>
                    {history_menu_open ? 'Hide past topics' : 'Show past topics'}
                  </Text>
                </Pressable>
                <Pressable
                  style={styles.secondary_button}
                  onPress={() => set_publish_history([])}
                >
                  <Text style={styles.secondary_button_text}>Clear history</Text>
                </Pressable>
              </View>

              {history_menu_open && publish_history.length > 0 ? (
                <View style={styles.dropdown_menu}>
                  {publish_history.map((topic) => (
                    <Pressable
                      key={topic}
                      style={styles.dropdown_item}
                      onPress={() => {
                        set_publish_topic(topic);
                        set_history_menu_open(false);
                      }}
                    >
                      <Text style={styles.dropdown_item_text}>{topic}</Text>
                    </Pressable>
                  ))}
                </View>
              ) : null}

              <LabeledField label="JSON message">
                <TextInput
                  value={publish_draft}
                  onChangeText={set_publish_draft}
                  multiline
                  textAlignVertical="top"
                  autoCapitalize="none"
                  autoCorrect={false}
                  spellCheck={false}
                  keyboardType={Platform.OS === 'ios' ? 'ascii-capable' : 'default'}
                  style={styles.editor}
                />
              </LabeledField>

              <Text
                style={[
                  styles.validation_text,
                  publish_json_error == null ? styles.validation_ok : styles.validation_error,
                ]}
              >
                {publish_json_error == null ? 'Valid JSON payload.' : publish_json_error}
              </Text>

              <Pressable
                style={[styles.primary_button, !can_send && styles.button_disabled]}
                onPress={handle_send_press}
                disabled={!can_send}
              >
                <Text style={styles.primary_button_text}>Send</Text>
              </Pressable>
            </View>
          ) : null}
        </ScrollView>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
}

function LabeledField({
  children,
  label,
}: {
  children: ReactNode;
  label: string;
}) {
  return (
    <View style={styles.field}>
      <Text style={styles.field_label}>{label}</Text>
      {children}
    </View>
  );
}

function StatusPill({ status }: { status: StatusInfo }) {
  return (
    <View style={[styles.status_pill, status_styles[status.tone]]}>
      <Text style={styles.status_pill_text}>{status.text}</Text>
    </View>
  );
}

function JsonTreeNode({
  default_expanded,
  label,
  level,
  value,
}: {
  default_expanded: boolean;
  label: string;
  level: number;
  value: unknown;
}) {
  const expandable = value != null && typeof value === 'object';
  const [expanded, set_expanded] = useState(default_expanded);

  useEffect(() => {
    set_expanded(default_expanded);
  }, [default_expanded]);

  const indentation = { marginLeft: level * 14 };

  if (!expandable) {
    return (
      <View style={[styles.tree_row, indentation]}>
        <Text style={styles.tree_label}>{label}: </Text>
        <Text style={styles.tree_value}>{format_scalar(value)}</Text>
      </View>
    );
  }

  const entries = Array.isArray(value)
    ? value.map((item, index) => [`[${index}]`, item] as const)
    : Object.entries(value as Record<string, unknown>);

  return (
    <View style={indentation}>
      <Pressable style={styles.tree_toggle} onPress={() => set_expanded((current) => !current)}>
        <Text style={styles.tree_toggle_icon}>{expanded ? '▾' : '▸'}</Text>
        <Text style={styles.tree_label}>{label}</Text>
        <Text style={styles.tree_summary}>{summarize_container(value)}</Text>
      </Pressable>
      {expanded
        ? entries.map(([child_label, child_value]) => (
            <JsonTreeNode
              key={`${label}-${child_label}`}
              default_expanded={default_expanded}
              label={child_label}
              level={level + 1}
              value={child_value}
            />
          ))
        : null}
    </View>
  );
}

function sanitize_bootstrap_payload(value: unknown): BootstrapPayload | null {
  if (value == null || typeof value !== 'object') {
    return null;
  }

  const candidate = value as Record<string, unknown>;
  if (
    typeof candidate.scheme !== 'string' ||
    typeof candidate.port !== 'number' ||
    typeof candidate.path !== 'string' ||
    !Array.isArray(candidate.addresses)
  ) {
    return null;
  }

  const addresses = candidate.addresses.filter(
    (entry): entry is string => typeof entry === 'string' && entry.length > 0
  );
  if (addresses.length === 0) {
    return null;
  }

  return {
    scheme: candidate.scheme,
    port: candidate.port,
    path: normalize_path(candidate.path),
    addresses,
  };
}

function parse_bootstrap_payload(raw: string): BootstrapPayload | null {
  try {
    const parsed = JSON.parse(raw) as Record<string, unknown>;
    if (
      typeof parsed.scheme !== 'string' ||
      typeof parsed.port !== 'number' ||
      typeof parsed.path !== 'string' ||
      !Array.isArray(parsed.addresses)
    ) {
      return null;
    }
    const addresses = parsed.addresses.filter((value): value is string => typeof value === 'string' && value.length > 0);
    if (addresses.length === 0) {
      return null;
    }
    return {
      scheme: parsed.scheme,
      port: parsed.port,
      path: normalize_path(parsed.path),
      addresses,
    };
  } catch {
    return null;
  }
}

function resolve_endpoint(
  raw_address: string,
  bootstrap_payload: BootstrapPayload | null
): { base_address: string; path: string } | null {
  const trimmed = raw_address.trim();
  const fallback_scheme = bootstrap_payload?.scheme ?? kDefaultBootstrap.scheme;
  const fallback_port = bootstrap_payload?.port ?? kDefaultBootstrap.port;
  const fallback_path = normalize_path(bootstrap_payload?.path ?? kDefaultBootstrap.path);

  if (trimmed.length === 0) {
    return null;
  }

  try {
    const candidate = trimmed.includes('://') ? trimmed : `${fallback_scheme}://${trimmed}`;
    const url = new URL(candidate);
    if (url.hostname.length === 0) {
      return null;
    }

    const is_http_like = url.protocol === 'http:' || url.protocol === 'https:';
    const scheme = is_http_like ? fallback_scheme : url.protocol === 'wss:' ? 'wss' : 'ws';
    const port = is_http_like
      ? fallback_port
      : url.port.length > 0
          ? Number(url.port)
          : fallback_port;
    if (!Number.isFinite(port) || port <= 0 || port > 65535) {
      return null;
    }
    const host = url.hostname.includes(':') ? `[${url.hostname}]` : url.hostname;
    const path = is_http_like
      ? fallback_path
      : normalize_path(url.pathname !== '/' ? url.pathname : fallback_path);
    return {
      base_address: `${scheme}://${host}:${port}`,
      path,
    };
  } catch {
    return null;
  }
}

function parse_browser_address(
  raw_address: string
): { protocol: string; hostname: string; port: string } | null {
  const trimmed = raw_address.trim();
  if (trimmed.length === 0) {
    return null;
  }

  try {
    const candidate = trimmed.includes('://') ? trimmed : `http://${trimmed}`;
    const url = new URL(candidate);
    return {
      protocol: url.protocol,
      hostname: url.hostname,
      port: url.port,
    };
  } catch {
    return null;
  }
}

function normalize_path(path: string): string {
  if (path.trim().length === 0) {
    return '/mads';
  }
  const prefixed = path.startsWith('/') ? path : `/${path}`;
  return prefixed.replace(/\/+$/, '') || '/mads';
}

function parse_subscription_topics(input: string): string[] {
  const items = input
    .split(',')
    .map((value) => value.trim())
    .filter((value, index, array) => value.length > 0 && array.indexOf(value) === index);
  if (items.length === 0 || items.includes('_all')) {
    return ['_all'];
  }
  return items;
}

function build_topic_url(base_address: string, path: string, topic: string): string {
  return `${base_address}${normalize_path(path)}/${encodeURIComponent(topic)}`;
}

function close_listen_sockets(listen_sockets_ref: React.MutableRefObject<WebSocket[]>) {
  for (const socket of listen_sockets_ref.current) {
    try {
      socket.close();
    } catch {
      // Ignore close failures while rotating sockets.
    }
  }
  listen_sockets_ref.current = [];
}

function close_publish_socket(publish_socket_ref: React.MutableRefObject<WebSocket | null>) {
  if (publish_socket_ref.current == null) {
    return;
  }
  try {
    publish_socket_ref.current.close();
  } catch {
    // Ignore close failures while rotating the publisher socket.
  }
  publish_socket_ref.current = null;
}

function normalize_incoming_message(
  topic: string,
  message: unknown
): { topic: string; message: unknown } {
  if (
    topic === '_all' &&
    message != null &&
    typeof message === 'object' &&
    typeof (message as Record<string, unknown>).topic === 'string' &&
    Object.prototype.hasOwnProperty.call(message, 'message')
  ) {
    return {
      topic: (message as Record<string, unknown>).topic as string,
      message: (message as Record<string, unknown>).message,
    };
  }

  return {
    topic,
    message,
  };
}

function stringify_error(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

function summarize_container(value: unknown): string {
  if (Array.isArray(value)) {
    return ` [${value.length}]`;
  }
  if (value != null && typeof value === 'object') {
    return ` {${Object.keys(value as Record<string, unknown>).length}}`;
  }
  return '';
}

function format_scalar(value: unknown): string {
  if (typeof value === 'string') {
    return `"${value}"`;
  }
  if (typeof value === 'number' || typeof value === 'boolean' || value == null) {
    return String(value);
  }
  return JSON.stringify(value);
}

const status_styles = StyleSheet.create({
  neutral: {
    backgroundColor: '#eee2ca',
    borderColor: '#d4b98d',
  },
  success: {
    backgroundColor: '#dff0d6',
    borderColor: '#8eb87d',
  },
  warning: {
    backgroundColor: '#f7e8b7',
    borderColor: '#d4b25f',
  },
  danger: {
    backgroundColor: '#f6d5d1',
    borderColor: '#d67a6e',
  },
});

const styles = StyleSheet.create({
  safe_area: {
    flex: 1,
    backgroundColor: '#e7dfd0',
  },
  shell: {
    flex: 1,
  },
  header: {
    paddingHorizontal: 22,
    paddingTop: 20,
    paddingBottom: 6,
    alignItems: 'center',
    gap: 10,
  },
  header_logo: {
    width: 220,
    height: 72,
  },
  header_title: {
    fontSize: 30,
    lineHeight: 34,
    fontWeight: '800',
    color: '#2a1d11',
    textAlign: 'center',
  },
  tab_bar: {
    flexDirection: 'row',
    gap: 10,
    paddingHorizontal: 18,
    paddingTop: 18,
  },
  tab_button: {
    flex: 1,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: '#cfbea3',
    backgroundColor: '#f5efe3',
    paddingVertical: 12,
    alignItems: 'center',
  },
  tab_button_active: {
    backgroundColor: '#2a1d11',
    borderColor: '#2a1d11',
  },
  tab_button_disabled: {
    opacity: 0.35,
  },
  tab_button_text: {
    fontSize: 15,
    fontWeight: '700',
    color: '#5c4f43',
  },
  tab_button_text_active: {
    color: '#fff7ea',
  },
  tab_button_text_disabled: {
    color: '#8f8475',
  },
  scroll: {
    flex: 1,
  },
  scroll_content: {
    padding: 18,
    paddingBottom: 28,
  },
  panel: {
    borderRadius: 26,
    backgroundColor: '#fbf7ef',
    borderWidth: 1,
    borderColor: '#d8cab2',
    padding: 18,
    gap: 16,
  },
  section_title: {
    fontSize: 24,
    fontWeight: '800',
    color: '#2a1d11',
  },
  section_body: {
    fontSize: 15,
    lineHeight: 22,
    color: '#5c4f43',
  },
  row: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 10,
    alignItems: 'center',
  },
  subscribe_row: {
    flexDirection: 'row',
    gap: 10,
    alignItems: 'stretch',
  },
  subscribe_input_wrap: {
    flex: 1,
  },
  subscribe_button: {
    minWidth: 124,
  },
  primary_button: {
    borderRadius: 16,
    backgroundColor: '#b65f2a',
    paddingHorizontal: 18,
    paddingVertical: 14,
    alignItems: 'center',
    justifyContent: 'center',
  },
  primary_button_text: {
    color: '#fff8ef',
    fontSize: 15,
    fontWeight: '800',
  },
  secondary_button: {
    borderRadius: 14,
    borderWidth: 1,
    borderColor: '#d0bf9f',
    backgroundColor: '#f5ede0',
    paddingHorizontal: 14,
    paddingVertical: 12,
  },
  secondary_button_text: {
    color: '#4c4033',
    fontSize: 14,
    fontWeight: '700',
  },
  button_disabled: {
    opacity: 0.45,
  },
  badge: {
    borderRadius: 999,
    backgroundColor: '#f1e0bf',
    paddingHorizontal: 12,
    paddingVertical: 8,
  },
  badge_text: {
    fontSize: 13,
    fontWeight: '700',
    color: '#6a512d',
  },
  field: {
    gap: 8,
  },
  field_label: {
    fontSize: 14,
    fontWeight: '700',
    color: '#4c4033',
  },
  input: {
    borderRadius: 16,
    borderWidth: 1,
    borderColor: '#d8cab2',
    backgroundColor: '#fffdf8',
    paddingHorizontal: 14,
    paddingVertical: 14,
    fontSize: 16,
    color: '#241910',
  },
  editor: {
    minHeight: 220,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: '#d8cab2',
    backgroundColor: '#241910',
    paddingHorizontal: 14,
    paddingVertical: 14,
    fontSize: 15,
    lineHeight: 22,
    color: '#f7f0e4',
    fontFamily: Platform.select({ ios: 'Menlo', android: 'monospace', default: 'monospace' }),
  },
  dropdown_block: {
    gap: 8,
  },
  dropdown_menu: {
    borderRadius: 16,
    borderWidth: 1,
    borderColor: '#d8cab2',
    backgroundColor: '#fffaf2',
    overflow: 'hidden',
  },
  dropdown_item: {
    paddingHorizontal: 14,
    paddingVertical: 14,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: '#dfd3c0',
  },
  dropdown_item_text: {
    fontSize: 15,
    color: '#2a1d11',
  },
  info_card: {
    borderRadius: 18,
    backgroundColor: '#f3ead9',
    padding: 14,
    gap: 4,
  },
  info_card_label: {
    fontSize: 12,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 1.2,
    color: '#866136',
  },
  info_card_value: {
    fontSize: 16,
    fontWeight: '700',
    color: '#2a1d11',
  },
  info_card_hint: {
    fontSize: 14,
    lineHeight: 20,
    color: '#5c4f43',
  },
  status_pill: {
    borderRadius: 16,
    borderWidth: 1,
    paddingHorizontal: 14,
    paddingVertical: 12,
  },
  status_pill_text: {
    fontSize: 14,
    lineHeight: 20,
    color: '#2a1d11',
  },
  empty_state: {
    fontSize: 15,
    lineHeight: 22,
    color: '#6f6150',
  },
  message_card: {
    borderRadius: 18,
    backgroundColor: '#f2ebe0',
    padding: 14,
  },
  tree_toggle: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    paddingVertical: 4,
  },
  tree_toggle_icon: {
    width: 16,
    fontSize: 15,
    color: '#8f6133',
  },
  tree_row: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    paddingVertical: 4,
  },
  tree_label: {
    fontSize: 14,
    fontWeight: '700',
    color: '#2a1d11',
  },
  tree_summary: {
    fontSize: 13,
    color: '#7c6d59',
  },
  tree_value: {
    fontSize: 14,
    color: '#4d4338',
    flexShrink: 1,
  },
  validation_text: {
    fontSize: 14,
    fontWeight: '600',
  },
  validation_ok: {
    color: '#2f6a31',
  },
  validation_error: {
    color: '#b3483d',
  },
  modal_root: {
    flex: 1,
    backgroundColor: '#16110d',
  },
  modal_header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingHorizontal: 18,
    paddingVertical: 16,
  },
  modal_title: {
    fontSize: 24,
    fontWeight: '800',
    color: '#fff7ea',
  },
  modal_close_button: {
    borderRadius: 12,
    borderWidth: 1,
    borderColor: '#8c7b67',
    paddingHorizontal: 12,
    paddingVertical: 8,
  },
  modal_close_text: {
    color: '#fff7ea',
    fontWeight: '700',
  },
  camera_shell: {
    flex: 1,
    margin: 18,
    borderRadius: 28,
    overflow: 'hidden',
    backgroundColor: '#000',
  },
  camera: {
    flex: 1,
  },
  camera_overlay: {
    ...StyleSheet.absoluteFillObject,
    justifyContent: 'center',
    alignItems: 'center',
    gap: 18,
    backgroundColor: 'rgba(0, 0, 0, 0.18)',
  },
  scan_frame: {
    width: 240,
    height: 240,
    borderRadius: 28,
    borderWidth: 3,
    borderColor: '#fff7ea',
    backgroundColor: 'transparent',
  },
  camera_hint: {
    paddingHorizontal: 28,
    textAlign: 'center',
    fontSize: 15,
    lineHeight: 22,
    color: '#fff7ea',
  },
});
