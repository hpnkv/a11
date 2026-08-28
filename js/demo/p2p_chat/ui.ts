/**
 * DOM rendering for the P2P chat room demo.
 *
 * Binds to elements prefixed with `p2p-` in the guide page HTML.
 * Renders chat messages, participant list with connection badges,
 * room controls, and error display.
 */

import { ConnectionType, type ChatMessage, type PeerInfo } from './types.js';

/** Callback signatures for user actions. */
export interface ChatUICallbacks {
  onSend: (text: string) => void;
  onSetName: (name: string) => void;
  onCreateRoom: () => void;
  onJoinRoom: (roomId: string) => void;
}

export class ChatUI {
  private readonly root: HTMLElement;
  private readonly lobbyView: HTMLElement;
  private readonly roomView: HTMLElement;
  private readonly messagesEl: HTMLElement;
  private readonly participantsEl: HTMLElement;
  private readonly roomLinkEl: HTMLElement;
  private readonly roomIdDisplay: HTMLElement;
  private readonly errorsEl: HTMLElement;
  private readonly messageInput: HTMLInputElement;
  private readonly nameInput: HTMLInputElement;
  private readonly joinInput: HTMLInputElement;
  private readonly statusEl: HTMLElement;

  private callbacks: ChatUICallbacks | null = null;
  private myId = '';

  constructor() {
    this.root = document.querySelector('#p2p-chat')!;
    this.lobbyView = this.root.querySelector('#p2p-lobby')!;
    this.roomView = this.root.querySelector('#p2p-room')!;
    this.messagesEl = this.root.querySelector('#p2p-messages')!;
    this.participantsEl = this.root.querySelector('#p2p-participants')!;
    this.roomLinkEl = this.root.querySelector('#p2p-room-link')!;
    this.roomIdDisplay = this.root.querySelector('#p2p-room-id')!;
    // Error element is at the top level of #p2p-chat, visible in
    // both lobby and room views.
    this.errorsEl = this.root.querySelector('#p2p-errors')!;
    this.messageInput = this.root.querySelector('#p2p-message-input')!;
    this.nameInput = this.root.querySelector('#p2p-name-input')!;
    this.joinInput = this.root.querySelector('#p2p-join-input')!;
    this.statusEl = this.root.querySelector('#p2p-status')!;

    this.bindEvents();
  }

  /** Set UI callbacks and the local peer ID. */
  bind(callbacks: ChatUICallbacks, myId: string): void {
    this.callbacks = callbacks;
    this.myId = myId;
  }

  /** Show the lobby (create/join) view. */
  showLobby(): void {
    this.lobbyView.style.display = '';
    this.roomView.style.display = 'none';
  }

  /** Show the active room view. */
  showRoom(hostId: string, shareUrl: string): void {
    this.lobbyView.style.display = 'none';
    // Override the CSS `display: none` with an explicit value.
    this.roomView.style.display = 'block';
    this.roomIdDisplay.textContent = hostId;
    this.roomLinkEl.textContent = shareUrl;
    (this.roomLinkEl as HTMLAnchorElement).href = shareUrl;
  }

  /** Update the connection status indicator. */
  setStatus(text: string): void {
    this.statusEl.textContent = text;
  }

  /** Render the chat message list. */
  renderMessages(messages: ChatMessage[], participants: Map<string, string | null>): void {
    this.messagesEl.replaceChildren(
      ...messages.map((msg) => {
        const bubble = document.createElement('div');
        const isOwn = msg.peerId === this.myId;
        bubble.className = `p2p-bubble${isOwn ? ' own' : ''}`;

        const header = document.createElement('div');
        header.className = 'p2p-bubble-header';
        const displayName = participants.get(msg.peerId) ?? msg.peerId;
        header.textContent = displayName;
        bubble.appendChild(header);

        const body = document.createElement('div');
        body.className = 'p2p-bubble-body';
        body.textContent = msg.text;
        bubble.appendChild(body);

        const time = document.createElement('time');
        time.className = 'p2p-bubble-time';
        const date = new Date(msg.timestamp);
        time.dateTime = date.toISOString();
        time.textContent = date.toLocaleTimeString([], { hour12: false });
        bubble.appendChild(time);

        return bubble;
      }),
    );
    this.messagesEl.scrollTop = this.messagesEl.scrollHeight;
  }

  /** Render the participant list with connection badges. */
  renderParticipants(peers: PeerInfo[]): void {
    this.participantsEl.replaceChildren(
      ...peers.map((peer) => {
        const row = document.createElement('div');
        row.className = 'p2p-participant';

        const badge = document.createElement('span');
        badge.className = `p2p-conn-badge ${peer.connectionType}`;
        badge.title = connectionLabel(peer.connectionType);
        badge.textContent = connectionIcon(peer.connectionType);
        row.appendChild(badge);

        const name = document.createElement('span');
        name.className = 'p2p-participant-name';
        const label = peer.name ?? peer.peerId;
        name.textContent = peer.peerId === this.myId ? `${label} (you)` : label;
        row.appendChild(name);

        if (peer.isHost) {
          const hostBadge = document.createElement('span');
          hostBadge.className = 'p2p-host-badge';
          hostBadge.textContent = 'host';
          row.appendChild(hostBadge);
        }

        if (peer.connectionType === ConnectionType.TURN) {
          const warn = document.createElement('span');
          warn.className = 'p2p-turn-warning';
          warn.textContent = 'TURN relayed';
          row.appendChild(warn);
        }

        return row;
      }),
    );
  }

  /** Show an error message. */
  showError(message: string): void {
    this.errorsEl.textContent = message;
  }

  /** Clear any error message. */
  clearError(): void {
    this.errorsEl.textContent = '';
  }

  // -------------------------------------------------------- events

  private bindEvents(): void {
    // Send message form.
    const sendForm = this.root.querySelector('#p2p-send-form');
    sendForm?.addEventListener('submit', (e) => {
      e.preventDefault();
      const text = this.messageInput.value.trim();
      if (text && this.callbacks) {
        this.callbacks.onSend(text);
        this.messageInput.value = '';
      }
    });

    // Set name form.
    const nameForm = this.root.querySelector('#p2p-name-form');
    nameForm?.addEventListener('submit', (e) => {
      e.preventDefault();
      const name = this.nameInput.value.trim();
      if (name && this.callbacks) {
        this.callbacks.onSetName(name);
      }
    });

    // Create room button.
    const createBtn = this.root.querySelector('#p2p-create-btn');
    createBtn?.addEventListener('click', () => {
      this.callbacks?.onCreateRoom();
    });

    // Join form — accepts a host peer ID or a full share URL.
    const joinForm = this.root.querySelector('#p2p-join-form');
    joinForm?.addEventListener('submit', (e) => {
      e.preventDefault();
      const raw = this.joinInput.value.trim();
      if (!raw || !this.callbacks) return;
      // Extract host ID from a full URL if the user pasted one.
      const hostId = extractHostId(raw);
      if (hostId) {
        this.callbacks.onJoinRoom(hostId);
      } else {
        // Treat the raw input as a host ID directly.
        this.callbacks.onJoinRoom(raw);
      }
    });

    // Copy link button.
    const copyBtn = this.root.querySelector('#p2p-copy-link');
    copyBtn?.addEventListener('click', () => {
      const url = this.roomLinkEl.textContent ?? '';
      void navigator.clipboard?.writeText(url).then(() => {
        if (copyBtn instanceof HTMLElement) {
          copyBtn.textContent = 'Copied!';
          setTimeout(() => { copyBtn.textContent = 'Copy link'; }, 2000);
        }
      });
    });
  }
}

function connectionIcon(type: ConnectionType): string {
  switch (type) {
    case ConnectionType.DIRECT: return '●';
    case ConnectionType.TURN: return '◆';
    case ConnectionType.UNKNOWN: return '○';
  }
}

function connectionLabel(type: ConnectionType): string {
  switch (type) {
    case ConnectionType.DIRECT: return 'Direct (STUN)';
    case ConnectionType.TURN: return 'TURN Relayed';
    case ConnectionType.UNKNOWN: return 'Connecting…';
  }
}

/** Extract a host peer ID from a share URL, or return null. */
function extractHostId(input: string): string | null {
  try {
    const url = new URL(input);
    return url.searchParams.get('host');
  } catch {
    return null;
  }
}
