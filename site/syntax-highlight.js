// Shared lexical highlighting for static examples and rendered Markdown.
// Sputnik rules follow frontend/lexer: # comments need preceding whitespace,
// // is division, and # in &Type#method is part of a method reference.
(() => {
  const escapeHtml = (text) => String(text).replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
  const token = (kind, text) => `<span class="tok-${kind}">${escapeHtml(text)}</span>`;
  const words = (text) => new Set(text.split(/\s+/));
  const keywords = {
    sputnik: words('and as async attr break case case! catch class class_method class_prop def do elif else elsif ensure export extend false from if import in include loop mixin not null or package prop raise rescue return self super then throw true try unless until when while var get set macro quote unquote schema numeric workflow step compensate require invariant property expect pure with frozen'),
    sh: words('if then else elif fi for in do done while until case esac function export local return exit source set unset true false'),
    json: words('true false null'),
    toml: words('true false'),
    ebnf: words('epsilon EOF NEWLINE INDENT DEDENT')
  };
  const aliases = {bash: 'sh', shell: 'sh', zsh: 'sh', ini: 'toml', plaintext: 'text', plain: 'text'};
  const identifier = /^[\p{L}_][\p{L}\p{N}_]*(?:[!?](?!=))?/u;
  const number = /^(?:0[xX][\da-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d[\d_]*)?)/;
  const operator = /^(?:\.\?\.|\.\.\.|\.\.|===|<=>|::=|==|!=|<=|>=|<<|>>|\*\*|\/\/|\?\?|&&|\|\||->|=>|[+*\/%=<>!&|^?:.#~-])/;

  const commentStarts = (source, index, lang) => ['sputnik', 'sh', 'toml'].includes(lang)
    && source[index] === '#' && (index === 0 || /\s/.test(source[index - 1]));

  const interpolationEnd = (source, start, lang) => {
    let depth = 1;
    for (let index = start; index < source.length; index++) {
      const char = source[index];
      if (char === '"' || char === "'") {
        index = quoted(source, index, lang).end - 1;
      } else if (commentStarts(source, index, lang)) {
        const newline = source.indexOf('\n', index);
        index = newline < 0 ? source.length : newline;
      } else if (char === '{') {
        depth++;
      } else if (char === '}' && --depth === 0) {
        return index;
      }
    }
    return -1;
  };

  const quoted = (source, start, lang) => {
    const quote = source[start];
    const delimiter = source.startsWith(quote.repeat(3), start) ? quote.repeat(3) : quote;
    const tagged = /[\p{L}\p{N}_]/u.test(source[start - 1] || '');
    const raw = /\braw\s*$/.test(source.slice(Math.max(0, start - 10), start));
    const interpolate = lang === 'sputnik' && !raw && (quote === '"' || tagged);
    const html = [];
    let segment = start;
    let index = start + delimiter.length;
    while (index < source.length) {
      if (source[index] === '\\') {
        index += Math.min(2, source.length - index);
      } else if (interpolate && source.startsWith('#{', index)) {
        const close = interpolationEnd(source, index + 2, lang);
        if (close < 0) { index++; continue; }
        if (segment < index) html.push(token('str', source.slice(segment, index)));
        html.push(token('interp', '#{'), highlight(source.slice(index + 2, close), lang), token('interp', '}'));
        index = close + 1;
        segment = index;
      } else if (source.startsWith(delimiter, index)) {
        index += delimiter.length;
        break;
      } else {
        index++;
      }
    }
    if (segment < index) html.push(token('str', source.slice(segment, index)));
    return {end: index, html: html.join('')};
  };

  const highlight = (source, language = 'sputnik') => {
    const name = language.toLowerCase();
    const lang = aliases[name] || name;
    if (!keywords[lang]) return escapeHtml(source);
    const html = [];
    let index = 0;
    let previous = '';
    while (index < source.length) {
      const rest = source.slice(index);
      const char = source[index];
      let match;
      if ((match = rest.match(/^\s+/))) {
        html.push(escapeHtml(match[0]));
        index += match[0].length;
        continue;
      }
      if (commentStarts(source, index, lang) && !source.startsWith('#{', index)) {
        const newline = source.indexOf('\n', index);
        const end = newline < 0 ? source.length : newline;
        html.push(token('comment', source.slice(index, end)));
        index = end;
        previous = '';
        continue;
      }
      if (char === '"' || char === "'") {
        const string = quoted(source, index, lang);
        const isJsonKey = lang === 'json' && /^\s*:/.test(source.slice(string.end));
        html.push(isJsonKey ? token('attr', source.slice(index, string.end)) : string.html);
        index = string.end;
        previous = 'string';
        continue;
      }
      if (lang === 'sh' && (match = rest.match(/^https?:\/\/[^\s"'<>]+/))) {
        html.push(token('str', match[0]));
      } else if (lang === 'sh' && (match = rest.match(/^--?[A-Za-z][\w-]*/))) {
        html.push(token('attr', match[0]));
      } else if ((match = rest.match(/^(?:\$(?:\{[^}\n]+\}|[\p{L}_][\p{L}\p{N}_]*|\d+)|@@?[\p{L}_][\p{L}\p{N}_]*|_[1-9]\d*)/u))) {
        html.push(token('var', match[0]));
      } else if (lang === 'sputnik' && char === ':' && (match = rest.slice(1).match(identifier))) {
        match = [':' + match[0]];
        html.push(token('symbol', match[0]));
      } else if ((match = rest.match(number))) {
        html.push(token('num', match[0]));
      } else if ((match = rest.match(lang === 'sh' ? /^[\p{L}_./][\p{L}\p{N}_./~-]*/u : identifier))) {
        const word = match[0];
        const following = source.slice(index + word.length);
        const atCommand = lang === 'sh' && /(?:^|\n|&&|\|\||;)\s*$/.test(source.slice(0, index));
        let kind = '';
        if (keywords[lang].has(word)) kind = 'key';
        else if (lang === 'sputnik' && /^\p{Lu}/u.test(word)) kind = 'type';
        else if (lang === 'sputnik' && (['def', '.', '.?.', '#'].includes(previous) || /^\s*\(/.test(following) || ['print', 'p', 'pp'].includes(word))) kind = 'fn';
        else if (lang === 'toml' && /^\s*=/.test(following)) kind = 'attr';
        else if (lang === 'ebnf' && /^\s*(?:::)?=/.test(following)) kind = 'fn';
        else if (atCommand) kind = 'fn';
        html.push(kind ? token(kind, word) : escapeHtml(word));
      } else if ((match = rest.match(operator))) {
        html.push(token('op', match[0]));
      } else {
        match = [char];
        html.push(escapeHtml(char));
      }
      previous = match[0];
      index += match[0].length;
    }
    return html.join('');
  };

  const highlightAll = (root = document) => {
    root.querySelectorAll('pre code:not([data-syntax-highlighted])').forEach((code) => {
      const language = [...code.classList].find(name => name.startsWith('language-'))?.slice(9) || 'sputnik';
      code.innerHTML = highlight(code.textContent, language);
      code.dataset.syntaxHighlighted = 'true';
    });
  };

  window.SputnikSyntax = {highlight, highlightAll};
})();
