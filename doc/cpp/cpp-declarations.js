(() => {
  "use strict";

  const keywords = new Set([
    "alignas", "alignof", "auto", "class", "concept", "const",
    "consteval", "constexpr", "constinit", "decltype", "delete", "enum",
    "explicit", "export", "extern", "final", "friend", "inline",
    "mutable", "namespace", "noexcept", "operator", "override", "private",
    "protected", "public", "requires", "static", "struct", "template",
    "thread_local", "typename", "union", "using", "virtual", "volatile",
  ]);
  const builtinTypes = new Set([
    "array", "bool", "char", "char8_t", "char16_t", "char32_t", "double",
    "float", "function", "int", "long", "map", "optional", "shared_ptr",
    "short", "signed", "size_t", "span", "string", "string_view",
    "unique_ptr", "unsigned", "variant", "vector", "void", "wchar_t",
  ]);
  const namespaces = new Set(["a11", "absl", "std", "thread"]);
  const constants = new Set(["false", "nullptr", "true"]);
  const tokenPattern = /(\/\/.*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b(?:0[xX][\da-fA-F]+|\d+(?:\.\d+)?)\b|\b[A-Za-z_]\w*\b|::|\.\.\.|&&|\|\||->\*|->|<=>|<<|>>|<=|>=|==|!=|[<>{}()[\],*&=+\-/%!~?:;])/g;

  function tokenClass(token, context, functionName) {
    if (token.startsWith("//") || token.startsWith("/*")) {
      return "cpp-comment";
    }
    if (token.startsWith('"') || token.startsWith("'")) return "cpp-string";
    if (/^\d/.test(token)) return "cpp-number";
    if (keywords.has(token)) return "cpp-keyword";
    if (constants.has(token)) return "cpp-constant";
    if (builtinTypes.has(token)) return "cpp-type";
    if (token === functionName) return "cpp-function";
    if (namespaces.has(token)) return "cpp-namespace";
    if (/^\W/.test(token)) return "cpp-operator";
    if (/^[A-Z]/.test(token) || context.closest(".paramtype")) {
      return "cpp-type";
    }
    return "";
  }

  function highlightTextNode(node, functionName) {
    const text = node.nodeValue;
    const matches = [...text.matchAll(tokenPattern)];
    if (matches.length === 0) return;

    const output = document.createDocumentFragment();
    let offset = 0;
    for (const match of matches) {
      output.append(text.slice(offset, match.index));
      const className = tokenClass(match[0], node.parentElement, functionName);
      if (className) {
        const span = document.createElement("span");
        span.className = className;
        span.textContent = match[0];
        output.append(span);
      } else {
        output.append(match[0]);
      }
      offset = match.index + match[0].length;
    }
    output.append(text.slice(offset));
    node.replaceWith(output);
  }

  function highlight(container, functionName = "") {
    const walker = document.createTreeWalker(container, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) highlightTextNode(node, functionName);
  }

  function alignParentheses(table) {
    const rows = [...table.rows];
    if (rows.length < 2) return;

    const lastName = rows.at(-1).querySelector("td.paramname");
    if (!lastName) return;
    const textNodes = [];
    const walker = document.createTreeWalker(lastName, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) textNodes.push(walker.currentNode);

    const closingNode = textNodes.reverse().find((node) =>
      node.nodeValue.includes(")"));
    if (!closingNode) return;
    const closeAt = closingNode.nodeValue.lastIndexOf(")");
    const suffix = closingNode.nodeValue.slice(closeAt + 1).trim();
    closingNode.nodeValue = closingNode.nodeValue.slice(0, closeAt);

    const row = table.insertRow();
    row.insertCell().className = "paramkey";
    const parenthesis = row.insertCell();
    parenthesis.className = "paramparen cpp-operator";
    parenthesis.textContent = ")";
    const trailing = row.insertCell();
    trailing.colSpan = 2;
    trailing.className = "paramtype";
    trailing.textContent = suffix;
  }

  function initialise() {
    for (const prototype of document.querySelectorAll(".memproto")) {
      const name = prototype.querySelector("td.memname")?.textContent
        .trim().match(/([A-Za-z_]\w*)$/)?.[1] ?? "";
      const table = prototype.querySelector("table.memname");
      if (table) alignParentheses(table);
      highlight(prototype, name);
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initialise);
  } else {
    initialise();
  }
})();
