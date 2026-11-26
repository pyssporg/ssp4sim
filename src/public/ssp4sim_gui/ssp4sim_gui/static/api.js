export class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    this.message = message;
  }
}

function errorMessage(error) {
  return error && typeof error.message === 'string'
    ? error.message
    : 'network request failed';
}

async function request(method, path, body) {
  const init = {
    method,
    headers: { Accept: 'application/json' },
  };
  if (body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }

  let response;
  try {
    response = await fetch(path, init);
  } catch (error) {
    throw new ApiError(0, 'network', errorMessage(error));
  }

  let text;
  try {
    text = await response.text();
  } catch (error) {
    throw new ApiError(response.status || 0, 'network', errorMessage(error));
  }

  let payload = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch (error) {
      if (response.ok) {
        throw new ApiError(
          response.status,
          'bad_response',
          'response body is not valid JSON',
        );
      }
    }
  }

  if (!response.ok) {
    const bodyError = payload && typeof payload === 'object' ? payload.error : null;
    const detail =
      payload && typeof payload === 'object' && typeof payload.detail === 'string'
        ? payload.detail
        : '';
    const code =
      bodyError && typeof bodyError.code === 'string' && bodyError.code
        ? bodyError.code
        : detail
          ? 'bad_request'
          : 'http_error';
    const message =
      bodyError && typeof bodyError.message === 'string' && bodyError.message
        ? bodyError.message
        : detail || response.statusText || `HTTP ${response.status}`;
    throw new ApiError(response.status, code, message);
  }

  return payload;
}

export async function getJSON(path) {
  return request('GET', path);
}

export async function postJSON(path, body) {
  return request('POST', path, body === undefined ? {} : body);
}

export async function deleteJSON(path) {
  return request('DELETE', path);
}

const NAMED_EVENTS = ['backend', 'state', 'time', 'done', 'error'];

export function connectEvents(onEvent, onStatus) {
  const source = new EventSource('/api/events');

  const handle = (event) => {
    if (!event || typeof event.data !== 'string') return;
    let payload;
    try {
      payload = JSON.parse(event.data);
    } catch (error) {
      return;
    }
    if (!payload || typeof payload !== 'object') return;
    const kind = typeof payload.kind === 'string' ? payload.kind : event.type;
    if (typeof onEvent === 'function') {
      onEvent({ kind, data: payload.data, at: payload.at });
    }
  };

  if (typeof source.addEventListener === 'function') {
    for (const kind of NAMED_EVENTS) {
      source.addEventListener(kind, handle);
    }
  }

  source.onopen = () => {
    if (typeof onStatus === 'function') onStatus('open');
  };
  source.onerror = (event) => {
    // The server also uses the SSE event name "error" for engine failures.
    // Those arrive as MessageEvents (with data); transport failures do not.
    if (event && typeof event.data === 'string') return;
    if (typeof onStatus === 'function') onStatus('reconnecting');
  };

  return {
    close() {
      try {
        source.close();
      } catch (error) {
        // Closing an already-closed EventSource is harmless.
      }
    },
  };
}
