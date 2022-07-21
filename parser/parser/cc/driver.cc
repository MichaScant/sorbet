#include <ruby_parser/driver.hh>
#include <ruby_parser/lexer.hh>

// Autogenerated code
#include "parser/parser/typedruby_debug_bison.h"
#include "parser/parser/typedruby_release_bison.h"

namespace ruby_parser {

base_driver::base_driver(ruby_version version, std::string_view source, sorbet::StableStringStorage<> &scratch,
                         const struct builder &builder, bool traceLexer, bool indentationAware)
    : build(builder), lex(diagnostics, version, source, scratch, traceLexer), pending_error(false), def_level(0),
      ast(nullptr), indentationAware(indentationAware) {}

const char *const base_driver::token_name(token_type type) {
    // We have several tokens that have the same human-readable string, but Bison won't
    // let us specify the same human-readable string for both of them (because bison
    // lets you use those names like ".." directory in production rules, which would
    // lead to ambiguity.
    //
    // Instead, we have this translation layer, which intercepts certain tokens and
    // displays their proper human-readable string.
    switch (type) {
        case token_type::tBDOT2:
            return "\"..\"";
        case token_type::tBDOT3:
            return "\"...\"";
        case token_type::tBACK_REF:
            return "\"`\"";
        case token_type::tAMPER2:
            return "\"&\"";
        case token_type::tSTAR2:
            return "\"*\"";
        case token_type::tLBRACK2:
            return "\"[\"";
        case token_type::tLPAREN2:
            return "\"(\"";
        case token_type::tCOLON3:
            return "\"::\"";
        case token_type::tPOW:
            return "\"**\"";
        case token_type::tUPLUS:
            return "\"unary +\"";
        case token_type::tUMINUS:
            return "\"unary -\"";
        default:
            return this->yytname[this->yytranslate(static_cast<int>(type))];
    }
}

void base_driver::rewind_and_reset(size_t newPos) {
    this->clear_lookahead();
    this->lex.rewind_and_reset_to_expr_end(newPos);
}

void base_driver::rewind_and_reset_to_beg(size_t newPos) {
    this->clear_lookahead();
    this->lex.rewind_and_reset_to_expr_beg(newPos);
}

void base_driver::rewind_if_dedented(token_t token, token_t endToken, bool force) {
    if (!force && !this->indentationAware) {
        return;
    }

    if (endToken->type() != token_type::tBEFORE_EOF && this->lex.compare_indent_level(token, endToken) <= 0) {
        return;
    }

    this->rewind_to_tok_start(endToken);

    const char *token_str_name = this->token_name(token->type());
    if (endToken->type() == token_type::tBEFORE_EOF) {
        this->diagnostics.emplace_back(dlevel::ERROR, dclass::EOFInsteadOfEnd, token, token_str_name);
    } else {
        this->diagnostics.emplace_back(dlevel::ERROR, dclass::DedentedEnd, token, token_str_name, endToken);
    }
}

bool base_driver::rewind_if_different_line(token_t token1, token_t token2) {
    if (token2->type() == token_type::tBEFORE_EOF) {
        // I tried to find a test that would pass a `tBEFORE_EOF` token to this function and couldn't
        // find one. Please add a test make any relevant changes to this method, and delete this raise.
        //
        // This is a user error not an ENFORCE because I'm worried it could infinitely loop if it
        // improperly handles tBEFORE_EOF, and that's a bad experience for the user.
        this->diagnostics.emplace_back(dlevel::ERROR, dclass::InternalError, token2,
                                       "rewind_if_different_line called on tBEFORE_EOF");
        return false;
    }

    if (!this->indentationAware) {
        return false;
    }

    if (token1->lineStart() == token2->lineStart()) {
        return false;
    }

    this->rewind_and_reset(token1->end());

    const char *token_str_name = this->token_name(token1->type());
    this->diagnostics.emplace_back(dlevel::ERROR, dclass::DefMissingName, token1, token_str_name, token2);
    return true;
}

// TODO(jez) This can quite easily get out of hand performance-wise. The major selling point of
// LR parsers is that they admit linear-time implementations.
//
// For the time being (read: until we start seeing performance problems in practice), introducing
// arbitrary-size backtracking like this method does is probably fine, because
//
// - It's only when there are syntax errors
// - Parse results are cached
// - It substantially improves the editor experience
//
// This backtracking makes the parser accidentally quadratic. Consider:
//
//     def f1
//       def f2
//         def f3
//     end
//
// The lexer and parser will analyze the source substring of f1 once, f2 twice, and f3 three times.
//
// A future extension might be to limit the number of bytes allowed to be reprocessed (say: all
// calls to rewind_and_reset when parsing a given file must move the lexer cursor by less than some
// multiple of the file size, or even than some magic constant).
//
// Most other uses of rewind_and_reset don't currently suffer as acutely from this problem, because
// they just back up over the last one or two tokens, not potentially back to the top of the file.
ForeignPtr base_driver::rewind_and_munge_body_if_dedented(SelfPtr self, token_t beginToken, size_t headerEndPos,
                                                          ForeignPtr body, token_t bodyStartToken,
                                                          token_t lastTokBeforeDedent, token_t endToken) {
    if (!this->indentationAware) {
        return body;
    }

    auto beginTokenLTEendToken = this->lex.compare_indent_level(beginToken, endToken) <= 0;
    if (beginTokenLTEendToken && endToken->type() != token_type::tBEFORE_EOF) {
        return body;
    }

    const char *token_str_name = this->token_name(beginToken->type());
    if (endToken->type() == token_type::tBEFORE_EOF) {
        this->diagnostics.emplace_back(dlevel::ERROR, dclass::EOFInsteadOfEnd, beginToken, token_str_name);
    } else {
        this->diagnostics.emplace_back(dlevel::ERROR, dclass::DedentedEnd, beginToken, token_str_name, endToken);
    }

    if (body == nullptr) {
        // Special case of "entire method was properly indented"
        // But bodyStartToken is tNL if empty body, which fails the assertion in compare_indent_level
        this->rewind_to_tok_start(endToken);
        return body;
    } else if (this->lex.compare_indent_level(bodyStartToken, beginToken) <= 0) {
        // Not even the very first thing in the body is indented. Treat this like emtpy method.
        this->rewind_and_reset(headerEndPos);
        auto emptyBody = this->build.compstmt(self, this->alloc.node_list());
        return emptyBody;
    } else if (lastTokBeforeDedent != nullptr) {
        // Something in the body is dedented. Only put the properly indented stmts in the method.
        auto truncatedBody = this->build.truncateBodyStmt(self, body, lastTokBeforeDedent);
        if (truncatedBody != nullptr) {
            this->rewind_and_reset(lastTokBeforeDedent->end());
            return truncatedBody;
        } else {
            // bodystmt had opt_else and/or opt_rescue; this is unhandled.
            // give up, and say that the method body was empty
            this->rewind_and_reset(headerEndPos);
            auto emptyBody = this->build.compstmt(self, this->alloc.node_list());
            return emptyBody;
        }
    } else {
        // Entire method body was properly indented, except for final kEND
        this->rewind_to_tok_start(endToken);
        return body;
    }
}

void base_driver::rewind_to_tok_start(token_t endToken) {
    if (endToken->type() == token_type::tBEFORE_EOF) {
        // Rewinding doesn't make sense here, because we're already at EOF (there's nothing left for ragel to scan).
        // Instead, just put the tBEFORE_EOF token back onto the queue so that other rules can use it.
        this->lex.unadvance(endToken);
    } else {
        this->rewind_and_reset(endToken->start());
    }
}

void base_driver::rewind_to_tok_end(token_t tok) {
    if (tok->type() == token_type::tBEFORE_EOF) {
        // Rewinding doesn't make sense here, because we're already at EOF (there's nothing left for ragel to scan).
        // Instead, just put the tBEFORE_EOF token back onto the queue so that other rules can use it.
        this->lex.unadvance(tok);
    } else {
        this->rewind_and_reset(tok->end());
    }
}

void base_driver::local_push() {
    lex.extend_static();
    lex.cmdarg.push(false);
    lex.cond.push(false);
    auto decls = alloc.node_list();
    bool staticContext = true;
    numparam_stack.push(decls, staticContext);
}

void base_driver::local_pop() {
    lex.unextend();
    lex.cmdarg.pop();
    lex.cond.pop();
    numparam_stack.pop();
}

typedruby_release::typedruby_release(std::string_view source, sorbet::StableStringStorage<> &scratch,
                                     const struct builder &builder, bool traceLexer, bool indentationAware)
    : base_driver(ruby_version::RUBY_31, source, scratch, builder, traceLexer, indentationAware) {}

ForeignPtr typedruby_release::parse(SelfPtr self, bool) {
    bison::typedruby_release::parser p(*this, self);
    p.parse();
    return ast;
}

typedruby_debug::typedruby_debug(std::string_view source, sorbet::StableStringStorage<> &scratch,
                                 const struct builder &builder, bool traceLexer, bool indentationAware)
    : base_driver(ruby_version::RUBY_31, source, scratch, builder, traceLexer, indentationAware) {}

ForeignPtr typedruby_debug::parse(SelfPtr self, bool traceParser) {
    bison::typedruby_debug::parser p(*this, self);
    p.set_debug_level(traceParser ? 1 : 0);
    p.parse();
    return ast;
}

} // namespace ruby_parser
