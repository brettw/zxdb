# TODO

- Currently we extract names of things from the structure of the declarations. Some programming
  languages or symbol output modes do not emit these and we have only names. In this case, I want to
  understand what we currently support and what the gaps are. For example, we probably need to
  handle this case by demangling the names and indexing that, and that will create cases where there
  are symbols with things like "::" in them in C++ where we don't know if the left side is a
  namespace, class, etc. Do we handle these cases correctly and if not, what do we need to do to
  support them well?
