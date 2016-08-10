namespace boost {
namespace http {
namespace reader {

namespace detail {

inline bool isalpha(unsigned char c)
{
    /* ALPHA          =  %x41-5A / %x61-7A   ; A-Z / a-z

       from Appendix B of RFC5234. */
    return (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A);
}

inline bool isdigit(unsigned char c)
{
    /* DIGIT          =  %x30-39   ; 0-9

       from Appendix B of RFC5234. */
    return c >= 0x30 && c <= 0x39;
}

inline bool isalnum(unsigned char c)
{
    return isalpha(c) || isdigit(c);
}

inline bool is_tchar(unsigned char c)
{
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~':
        return true;
    default:
        return isalnum(c);
    }
}

inline bool is_sp(unsigned char c)
{
    return c == '\x20';
}

inline bool is_vchar(unsigned char c)
{
    return c >= '\x21' && c <= '\x7E';
}

inline bool is_obs_text(unsigned char c)
{
    return c >= 0x80 && c <= 0xFF;
}

inline bool is_request_target_char(unsigned char c)
{
    switch (c) {
    case '?': case '/': case '-': case '.': case '_': case '~': case '%':
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*':
    case '+': case ',': case ';': case '=': case ':': case '@':
        return true;
    default:
        return isalnum(c);
    }
}

inline bool is_ows(unsigned char c)
{
    switch (c) {
    case '\x20':
    case '\x09':
        return true;
    default:
        return false;
    }
}

/* all valid field value characters except OWS */
inline bool is_nonnull_field_value_char(unsigned char c)
{
    return is_vchar(c) || is_obs_text(c);
}

inline bool is_field_value_char(unsigned char c)
{
    return is_nonnull_field_value_char(c) || is_ows(c);
}

inline bool is_chunk_ext_char(unsigned char c)
{
    switch (c) {
    case ';':
    case '=':
    case '"':
    case '\t':
    case ' ':
    case '!':
    case '\\':
        return true;
    default:
        return is_tchar(c) || (c >= 0x23 && c <= 0x5B)
            || (c >= 0x5D && c <= 0x7E) || is_obs_text(c) || is_vchar(c);
    }
}

enum FromDecimalString {
    DECSTRING_INVALID,
    DECSTRING_OK,
    DECSTRING_OVERFLOW
};

template<class Target>
FromDecimalString from_decimal_string(string_ref in, Target &out)
{
    out = 0;

    while (in.size() && in[0] == '0')
        in.remove_prefix(1);

    if (in.size() == 0)
        return DECSTRING_OK;

    Target digit = 1;

    for ( std::size_t i = in.size() - 1 ; ; ) {
        Target value;

        switch (in[i]) {
        case '0': case '1': case '2': case '3': case '4': case '5': case '6':
        case '7': case '8': case '9':
            value = in[i] - '0';
            break;
        default:
            return DECSTRING_INVALID;
        }

        if (std::numeric_limits<Target>::max() / digit < value)
            return DECSTRING_OVERFLOW;

        value *= digit;

        if (std::numeric_limits<Target>::max() - value < out)
            return DECSTRING_OVERFLOW;

        out += value;

        if (i == 0) {
            break;
        } else {
            if (std::numeric_limits<Target>::max() / 10 < digit)
                return DECSTRING_OVERFLOW;
            else
                digit *= 10;

            --i;
        }
    }
    return DECSTRING_OK;
}

enum DecodeTransferEncodingResult {
    CHUNKED_NOT_FOUND,
    CHUNKED_AT_END,
    CHUNKED_INVALID
};

struct decode_transfer_encoding_p
{
    struct state
    {
        state()
            : count(0)
            , res(CHUNKED_NOT_FOUND)
        {}

        unsigned count;
        DecodeTransferEncodingResult res;
    };

    static const bool STOP_ITER = true;
    static const bool PROC_ITER = false;

    decode_transfer_encoding_p(state &s)
        : count(s.count)
        , res(s.res)
    {}

    bool operator()(string_ref v) const
    {
        using boost::algorithm::iequals;

        // All transfer-coding names are case-insensitive (section 4 of RFC7230)
        if (!iequals(v, "chunked")) {
            if (count == 1) {
                /* If any transfer coding other than chunked is applied to a
                   request payload body, the sender MUST apply chunked as the
                   final transfer coding (section 3.3.1 of RFC7230) */
                res = CHUNKED_INVALID;
                return STOP_ITER;
            }

            return PROC_ITER;
        }

        ++count;

        if (count == 2) {
            /* A sender MUST NOT apply chunked more than once to a message body
               (section 3.3.1 of RFC7230) */
            res = CHUNKED_INVALID;
            return STOP_ITER;
        }

        res = CHUNKED_AT_END;
        return PROC_ITER;
    }

    unsigned &count;
    DecodeTransferEncodingResult &res;
};

inline DecodeTransferEncodingResult decode_transfer_encoding(string_ref field)
{
    decode_transfer_encoding_p::state p_state;
    decode_transfer_encoding_p p(p_state);
    header_value_any_of(field, p);
    return p.res;
}

} // namespace detail

inline
request::request()
    : body_type(NO_BODY)
    , state(EXPECT_METHOD)
    , code_(token::code::error_insufficient_data)
    , idx(0)
    , token_size_(0)
{}

inline void request::reset()
{
    body_type = NO_BODY;
    state = EXPECT_METHOD;
    code_ = token::code::error_insufficient_data;
    idx = 0;
    token_size_ = 0;
    ibuffer = asio::const_buffer();
}

inline token::code::value request::code() const
{
    return code_;
}

inline
request::size_type request::token_size() const
{
    return token_size_;
}

template<>
request::view_type request::value<token::method>() const
{
    assert(code_ == token::method::code);
    return view_type(asio::buffer_cast<const char*>(ibuffer) + idx,
                     token_size_);
}

template<>
request::view_type request::value<token::request_target>() const
{
    assert(code_ == token::request_target::code);
    return view_type(asio::buffer_cast<const char*>(ibuffer) + idx,
                     token_size_);
}

template<>
int request::value<token::version>() const
{
    assert(code_ == token::version::code);
    return *(asio::buffer_cast<const char*>(ibuffer) + idx) - '0';
}

template<>
request::view_type request::value<token::field_name>() const
{
    assert(code_ == token::field_name::code);
    return view_type(asio::buffer_cast<const char*>(ibuffer) + idx,
                     token_size_);
}

template<>
request::view_type request::value<token::field_value>() const
{
    assert(code_ == token::field_value::code);

    /* The field value does not include any leading or trailing whitespace: OWS
       occurring before the first non-whitespace octet of the field value or
       after the last non-whitespace octet of the field value ought to be
       excluded by parsers when extracting the field value from a header field
       (section 3.2.4 of RFC7230).

       OWS can happen in the middle of the field value too. Therefore, we can
       only detect leading OWS ahead of time (i.e. when only part of the field
       has been received) and ending OWS must be removed once the whole field
       has been received (i.e. a job to this layer of abstraction). */
    std::size_t ending_ows = 0;
    {
        const unsigned char *view
            = asio::buffer_cast<const unsigned char*>(ibuffer);
        for (size_t i = idx + token_size_ - 1 ; i > idx ; --i) {
            if (!detail::is_ows(view[i]))
                break;
            else
                ++ending_ows;
        }
    }

    return view_type(asio::buffer_cast<const char*>(ibuffer) + idx,
                     token_size_ - ending_ows);
}

template<>
asio::const_buffer request::value<token::body_chunk>() const
{
    assert(code_ == token::body_chunk::code);
    return asio::buffer(ibuffer + idx, token_size_);
}

inline token::code::value request::expected_token() const
{
    switch (state) {
    case ERRORED:
        return code_;
    case EXPECT_METHOD:
        return token::code::method;
    case EXPECT_SP_AFTER_METHOD:
    case EXPECT_STATIC_STR_AFTER_TARGET:
    case EXPECT_CRLF_AFTER_VERSION:
    case EXPECT_COLON:
    case EXPECT_CRLF_AFTER_HEADERS:
    case EXPECT_OWS_AFTER_COLON:
    case EXPECT_CRLF_AFTER_FIELD_VALUE:
    case EXPECT_CHUNK_SIZE:
    case EXPECT_CHUNK_EXT:
    case EXPEXT_CRLF_AFTER_CHUNK_EXT:
    case EXPECT_CRLF_AFTER_CHUNK_DATA:
    case EXPECT_TRAILER_COLON:
    case EXPECT_OWS_AFTER_TRAILER_COLON:
    case EXPECT_CRLF_AFTER_TRAILER_VALUE:
    case EXPECT_CRLF_AFTER_TRAILERS:
        return token::code::skip;
    case EXPECT_REQUEST_TARGET:
        return token::code::request_target;
    case EXPECT_VERSION:
        return token::code::version;
    case EXPECT_FIELD_NAME:
    case EXPECT_TRAILER_NAME:
        return token::code::field_name;
    case EXPECT_FIELD_VALUE:
    case EXPECT_TRAILER_VALUE:
        return token::code::field_value;
    case EXPECT_BODY:
    case EXPECT_CHUNK_DATA:
        return token::code::body_chunk;
    case EXPECT_END_OF_BODY:
        return token::code::end_of_body;
    case EXPECT_END_OF_MESSAGE:
        return token::code::end_of_message;
    }
}

inline void request::set_buffer(asio::const_buffer ibuffer)
{
    this->ibuffer = ibuffer;
    idx = 0;
}

inline void request::next()
{
    if (state == ERRORED)
        return;

    // This is a 0-sized token. Therefore, it is handled sooner.
    switch (state) {
    case EXPECT_END_OF_BODY:
        state = EXPECT_END_OF_MESSAGE;
        code_ = token::code::end_of_body;
        idx += token_size_;
        token_size_ = 0;
        return;
    case EXPECT_END_OF_MESSAGE:
        body_type = NO_BODY;
        state = EXPECT_METHOD;
        code_ = token::code::end_of_message;
        idx += token_size_;
        token_size_ = 0;
        return;
    default:
        break;
    }

    if (code_ != token::code::error_insufficient_data) {
        idx += token_size_;
        token_size_ = 0;
        code_ = token::code::error_insufficient_data;
    }

    if (idx == asio::buffer_size(ibuffer))
        return;

    switch (state) {
    case EXPECT_METHOD:
        {
            size_type i = idx + token_size_;
            for ( ; i != asio::buffer_size(ibuffer) ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (!detail::is_tchar(c)) {
                    if (i != idx) {
                        state = EXPECT_SP_AFTER_METHOD;
                        code_ = token::code::method;
                        token_size_ = i - idx;
                    } else {
                        state = ERRORED;
                        code_ = token::code::error_invalid_data;
                    }
                    return;
                }
            }
            token_size_ = i - idx;
            return;
        }
    case EXPECT_SP_AFTER_METHOD:
        {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[idx];
            if (detail::is_sp(c)) {
                state = EXPECT_REQUEST_TARGET;
                code_ = token::code::skip;
                token_size_ = 1;
            } else {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            }
            return;
        }
    case EXPECT_REQUEST_TARGET:
        for (size_type i = idx + token_size_ ; i != asio::buffer_size(ibuffer)
                 ; ++i) {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
            if (!detail::is_request_target_char(c)) {
                if (i != idx) {
                    state = EXPECT_STATIC_STR_AFTER_TARGET;
                    code_ = token::code::request_target;
                    token_size_ = i - idx;
                } else {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                }
                return;
            }
        }
        token_size_ = asio::buffer_size(ibuffer) - idx;
        return;
    case EXPECT_STATIC_STR_AFTER_TARGET:
        {
            unsigned char skip[] = {' ', 'H', 'T', 'T', 'P', '/', '1', '.'};
            size_type i = idx + token_size_;
            size_type count = std::min(asio::buffer_size(ibuffer),
                                       idx + sizeof(skip));
            for ( ; i != count ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (c != skip[i - idx]) {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                    return;
                }
            }
            token_size_ = i - idx;
            if (token_size_ == sizeof(skip)) {
                state = EXPECT_VERSION;
                code_ = token::code::skip;
            }
            return;
        }
    case EXPECT_VERSION:
        {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[idx];
            if (c < '0' || c > '9') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                state = EXPECT_CRLF_AFTER_VERSION;
                code_ = token::code::version;
                token_size_ = 1;

                version
                    = (value<token::version>() == 0)
                    ? HTTP_1_0 : NOT_HTTP_1_0_AND_HOST_NOT_READ;
            }
            return;
        }
    case EXPECT_CRLF_AFTER_VERSION:
        {
            unsigned char skip[] = {'\r', '\n'};
            size_type i = idx + token_size_;
            size_type count = std::min(asio::buffer_size(ibuffer),
                                       idx + sizeof(skip));
            for ( ; i != count ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (c != skip[i - idx]) {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                    return;
                }
            }
            token_size_ = i - idx;
            if (token_size_ == sizeof(skip)) {
                state = EXPECT_FIELD_NAME;
                code_ = token::code::skip;
            }
            return;
        }
    case EXPECT_FIELD_NAME:
        for (size_type i = idx + token_size_ ; i != asio::buffer_size(ibuffer)
                 ; ++i) {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
            if (!detail::is_tchar(c)) {
                if (i != idx) {
                    using boost::algorithm::iequals;

                    state = EXPECT_COLON;
                    code_ = token::code::field_name;
                    token_size_ = i - idx;

                    /* The only possible values for `body_type` at this time
                       are:

                       - NO_BODY
                       - CONTENT_LENGTH_READ
                       - CHUNKED_ENCODING_READ
                       - RANDOM_ENCODING_READ */
                    string_ref field = value<token::field_name>();
                    if (version == NOT_HTTP_1_0_AND_HOST_NOT_READ
                        && iequals(field, "Host")) {
                        version = NOT_HTTP_1_0_AND_HOST_READ;
                    } else if (iequals(field, "Transfer-Encoding")) {
                        switch (body_type) {
                        case CONTENT_LENGTH_READ:
                            /* Transfer-Encoding overrides Content-Length
                               (section 3.3.3 of RFC7230) */
                        case NO_BODY:
                        case RANDOM_ENCODING_READ:
                            body_type = READING_ENCODING;
                            break;
                        case CHUNKED_ENCODING_READ:
                            state = ERRORED;
                            code_
                                = token::code::error_invalid_transfer_encoding;
                            return;
                        default:
                            BOOST_HTTP_DETAIL_UNREACHABLE("");
                        }
                    } else if (iequals(field, "Content-Length")) {
                        switch (body_type) {
                        case NO_BODY:
                            body_type = READING_CONTENT_LENGTH;
                            break;
                        case CONTENT_LENGTH_READ:
                            state = ERRORED;
                            code_ = token::code::error_invalid_content_length;
                            return;
                        case CHUNKED_ENCODING_READ:
                        case RANDOM_ENCODING_READ:
                            /* Transfer-Encoding overrides Content-Length
                               (section 3.3.3 of RFC7230) */
                            break;
                        default:
                            BOOST_HTTP_DETAIL_UNREACHABLE("");
                        }
                    }
                } else if (c == '\r') {
                    state = EXPECT_CRLF_AFTER_HEADERS;
                    next();
                } else {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                }
                return;
            }
        }
        token_size_ = asio::buffer_size(ibuffer) - idx;
        return;
    case EXPECT_COLON:
        {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[idx];
            if (c != ':') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
                return;
            }
            state = EXPECT_OWS_AFTER_COLON;
            code_ = token::code::skip;

            size_type i = idx + 1;
            for ( ; i != asio::buffer_size(ibuffer) ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (!detail::is_ows(c)) {
                    state = EXPECT_FIELD_VALUE;
                    break;
                }
            }
            token_size_ = i - idx;
            return;
        }
    case EXPECT_OWS_AFTER_COLON:
        {
            size_type i = idx + token_size_;
            for ( ; i != asio::buffer_size(ibuffer) ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (!detail::is_ows(c)) {
                    state = EXPECT_FIELD_VALUE;
                    break;
                }
            }
            code_ = token::code::skip;
            token_size_ = i - idx;

            if (token_size_ == 0)
                return next();

            return;
        }
    case EXPECT_FIELD_VALUE:
        for (size_type i = idx + token_size_ ; i != asio::buffer_size(ibuffer)
                 ; ++i) {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
            if (!detail::is_field_value_char(c)) {
                if (i != idx) {
                    state = EXPECT_CRLF_AFTER_FIELD_VALUE;
                    code_ = token::code::field_value;
                    token_size_ = i - idx;

                    string_ref field = value<token::field_value>();
                    switch (body_type) {
                    case READING_CONTENT_LENGTH:
                        body_type = CONTENT_LENGTH_READ;
                        /* If a message is received that has multiple
                           Content-Length header fields with field-values
                           consisting of the same decimal value, or a single
                           Content-Length header field with a field value
                           containing a list of identical decimal values (e.g.,
                           "Content-Length: 42, 42"), indicating that duplicate
                           Content-Length header fields have been generated or
                           combined by an upstream message processor, then the
                           recipient MUST either reject the message [...]
                           (section 3.3.2 of RFC7230).

                           If a message is received without Transfer-Encoding
                           and with either multiple Content-Length header fields
                           having differing field-values or a single
                           Content-Length header field having an invalid value,
                           then the message framing is invalid and the recipient
                           MUST treat it as an unrecoverable error. If this is a
                           request message, the server MUST respond with a 400
                           (Bad Request) status code and then close the
                           connection (section 3.3.3 of RFC7230).

                           A sender MUST NOT send a Content-Length header field
                           in any message that contains a Transfer-Encoding
                           header field (section 3.3.2 of RFC7230).

                           Under the above rules, it's valid to reject messages
                           with improper Content-Length header field even if
                           it'd be possible to decode the message after some
                           future Transfer-Encoding header field is received. We
                           follow this shortcut to allow a much cheaper
                           implementation where less state is kept around. The
                           logic to handle such erroneous message shouldn't
                           impact the performance of the interaction with a
                           proper and conforming HTTP participant. And we should
                           minimmize the DoS attack surface too. */
                        switch (detail::from_decimal_string(field, body_size)) {
                        case detail::DECSTRING_INVALID:
                            state = ERRORED;
                            code_ = token::code::error_invalid_content_length;
                            break;
                        case detail::DECSTRING_OVERFLOW:
                            state = ERRORED;
                            code_ = token::code::error_content_length_overflow;
                            break;
                        case detail::DECSTRING_OK:
                            break;
                        }
                        break;
                    case READING_ENCODING:
                        switch (detail::decode_transfer_encoding(field)) {
                        case detail::CHUNKED_INVALID:
                            state = ERRORED;
                            code_
                                = token::code::error_invalid_transfer_encoding;
                            break;
                        case detail::CHUNKED_NOT_FOUND:
                            body_type = RANDOM_ENCODING_READ;
                            break;
                        case detail::CHUNKED_AT_END:
                            body_type = CHUNKED_ENCODING_READ;
                        }
                        break;
                    default:
                        break;
                    }
                } else {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                }
                return;
            }
        }
        token_size_ = asio::buffer_size(ibuffer) - idx;
        return;
    case EXPECT_CRLF_AFTER_FIELD_VALUE:
        if (idx + 1 >= asio::buffer_size(ibuffer))
            return;

        {
            const char *view = asio::buffer_cast<const char*>(ibuffer);
            if (view[idx] != '\r' || view[idx + 1] != '\n') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                state = EXPECT_FIELD_NAME;
                code_ = token::code::skip;
                token_size_ = 2;
            }
            return;
        }
    case EXPECT_CRLF_AFTER_HEADERS:
        if (idx + 1 >= asio::buffer_size(ibuffer))
            return;

        {
            const char *view = asio::buffer_cast<const char*>(ibuffer);
            if (view[idx] != '\r' || view[idx + 1] != '\n') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                if (version == NOT_HTTP_1_0_AND_HOST_NOT_READ) {
                    state = ERRORED;
                    code_ = token::code::error_no_host;
                    return;
                }

                switch (body_type) {
                case RANDOM_ENCODING_READ:
                    state = ERRORED;
                    code_ = token::code::error_invalid_transfer_encoding;
                    return;
                case NO_BODY:
                    state = EXPECT_END_OF_BODY;
                    break;
                case CHUNKED_ENCODING_READ:
                    state = EXPECT_CHUNK_SIZE;
                    break;
                case CONTENT_LENGTH_READ:
                    state = EXPECT_BODY;
                    break;
                default:
                    BOOST_HTTP_DETAIL_UNREACHABLE("READING_* variants should be"
                                                  " cleared when the field"
                                                  " value is read");
                }
                code_ = token::code::end_of_headers;
                token_size_ = 2;
            }
            return;
        }
    case EXPECT_BODY:
        code_ = token::code::body_chunk;
        {
            typedef common_type<std::size_t, uint_least64_t>::type Largest;

            token_size_ = std::min<Largest>(asio::buffer_size(ibuffer + idx),
                                            body_size);
            body_size -= token_size_;

            if (body_size == 0)
                state = EXPECT_END_OF_BODY;

            return;
        }
    case EXPECT_END_OF_BODY:
        BOOST_HTTP_DETAIL_UNREACHABLE("This state is handled sooner");
    case EXPECT_END_OF_MESSAGE:
        BOOST_HTTP_DETAIL_UNREACHABLE("This state is handled sooner");
    case EXPECT_CHUNK_SIZE:
        {
            typedef syntax::chunk_size<unsigned char> chunk_size;
            typedef chunk_size::view_type view_type;

            asio::const_buffer buf = ibuffer + idx;
            view_type in(asio::buffer_cast<const unsigned char*>(buf),
                         asio::buffer_size(buf));
            std::size_t nmatched = chunk_size::match(in);

            if (nmatched == 0) {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
                return;
            }

            if (nmatched == in.size())
                return;

            switch (native_value(chunk_size::decode(in.substr(0, nmatched),
                                                    body_size))) {
            case chunk_size::result::invalid:
                state = ERRORED;
                code_ = token::code::error_invalid_data;
                return;
            case chunk_size::result::overflow:
                state = ERRORED;
                code_ = token::code::error_chunk_size_overflow;
                return;
            case chunk_size::result::ok:
                state = EXPECT_CHUNK_EXT;
                code_ = token::code::skip;
                token_size_ = nmatched;
                return;
            };
            BOOST_HTTP_DETAIL_UNREACHABLE("internal error. UB?");
        }
    case EXPECT_CHUNK_EXT:
        {
            size_type i = idx + token_size_;
            for ( ; i != asio::buffer_size(ibuffer) ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (detail::is_chunk_ext_char(c))
                    continue;

                if (c != '\r') {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                    return;
                }

                state = EXPEXT_CRLF_AFTER_CHUNK_EXT;
                token_size_ = i - idx;

                if (token_size_ == 0)
                    return next();

                code_ = token::code::skip;
                return;
            }
            token_size_ = i - idx;
            return;
        }
    case EXPEXT_CRLF_AFTER_CHUNK_EXT:
        if (idx + 1 >= asio::buffer_size(ibuffer))
            return;

        {
            const char *view = asio::buffer_cast<const char*>(ibuffer);
            if (view[idx] != '\r' || view[idx + 1] != '\n') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                if (body_size != 0) {
                    state = EXPECT_CHUNK_DATA;
                    code_ = token::code::skip;
                } else {
                    state = EXPECT_TRAILER_NAME;
                    code_ = token::code::end_of_body;
                }

                token_size_ = 2;
            }
            return;
        }
    case EXPECT_CHUNK_DATA:
        code_ = token::code::body_chunk;
        {
            typedef common_type<std::size_t, uint_least64_t>::type Largest;

            token_size_ = std::min<Largest>(asio::buffer_size(ibuffer + idx),
                                            body_size);
            body_size -= token_size_;

            if (body_size == 0)
                state = EXPECT_CRLF_AFTER_CHUNK_DATA;

            return;
        }
    case EXPECT_CRLF_AFTER_CHUNK_DATA:
        if (idx + 1 >= asio::buffer_size(ibuffer))
            return;

        {
            const char *view = asio::buffer_cast<const char*>(ibuffer);
            if (view[idx] != '\r' || view[idx + 1] != '\n') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                state = EXPECT_CHUNK_SIZE;
                code_ = token::code::skip;
                token_size_ = 2;
            }
            return;
        }
    case EXPECT_TRAILER_NAME:
        for (size_type i = idx + token_size_ ; i != asio::buffer_size(ibuffer)
                 ; ++i) {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
            if (!detail::is_tchar(c)) {
                if (i != idx) {
                    state = EXPECT_TRAILER_COLON;
                    code_ = token::code::field_name;
                    token_size_ = i - idx;
                } else if (c == '\r') {
                    state = EXPECT_CRLF_AFTER_TRAILERS;
                    next();
                } else {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                }
                return;
            }
        }
        token_size_ = asio::buffer_size(ibuffer) - idx;
        return;
    case EXPECT_TRAILER_COLON:
        {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[idx];
            if (c != ':') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
                return;
            }
            state = EXPECT_OWS_AFTER_TRAILER_COLON;
            code_ = token::code::skip;

            size_type i = idx + 1;
            for ( ; i != asio::buffer_size(ibuffer) ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (!detail::is_ows(c)) {
                    state = EXPECT_TRAILER_VALUE;
                    break;
                }
            }
            token_size_ = i - idx;
            return;
        }
    case EXPECT_OWS_AFTER_TRAILER_COLON:
        {
            size_type i = idx + token_size_;
            for ( ; i != asio::buffer_size(ibuffer) ; ++i) {
                unsigned char c
                    = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
                if (!detail::is_ows(c)) {
                    state = EXPECT_TRAILER_VALUE;
                    break;
                }
            }
            code_ = token::code::skip;
            token_size_ = i - idx;

            if (token_size_ == 0)
                return next();

            return;
        }
    case EXPECT_TRAILER_VALUE:
        for (size_type i = idx + token_size_ ; i != asio::buffer_size(ibuffer)
                 ; ++i) {
            unsigned char c
                = asio::buffer_cast<const unsigned char*>(ibuffer)[i];
            if (!detail::is_field_value_char(c)) {
                if (i != idx) {
                    state = EXPECT_CRLF_AFTER_TRAILER_VALUE;
                    code_ = token::code::field_value;
                    token_size_ = i - idx;
                } else {
                    state = ERRORED;
                    code_ = token::code::error_invalid_data;
                }
                return;
            }
        }
        token_size_ = asio::buffer_size(ibuffer) - idx;
        return;
    case EXPECT_CRLF_AFTER_TRAILER_VALUE:
        if (idx + 1 >= asio::buffer_size(ibuffer))
            return;

        {
            const char *view = asio::buffer_cast<const char*>(ibuffer);
            if (view[idx] != '\r' || view[idx + 1] != '\n') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                state = EXPECT_TRAILER_NAME;
                code_ = token::code::skip;
                token_size_ = 2;
            }
            return;
        }
    case EXPECT_CRLF_AFTER_TRAILERS:
        if (idx + 1 >= asio::buffer_size(ibuffer))
            return;

        {
            const char *view = asio::buffer_cast<const char*>(ibuffer);
            if (view[idx] != '\r' || view[idx + 1] != '\n') {
                state = ERRORED;
                code_ = token::code::error_invalid_data;
            } else {
                body_type = NO_BODY;
                state = EXPECT_METHOD;
                code_ = token::code::end_of_message;
                token_size_ = 2;
            }
            return;
        }
    case ERRORED:
        break;
    }
    BOOST_HTTP_DETAIL_UNREACHABLE("The function should have already returned"
                                  " once the token was determined");
}

} // namespace reader
} // namespace boost
} // namespace http