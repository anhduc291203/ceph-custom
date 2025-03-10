// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#ifndef CEPH_RGW_KEYSTONE_H
#define CEPH_RGW_KEYSTONE_H

#include <type_traits>
#include <string_view>

#include <boost/optional.hpp>

#include "rgw_common.h"
#include "rgw_http_client.h"
#include "common/ceph_mutex.h"
#include "global/global_init.h"

#include <atomic>

bool rgw_is_pki_token(const std::string& token);
void rgw_get_token_id(const std::string& token, std::string& token_id);
static inline std::string rgw_get_token_id(const std::string& token)
{
  std::string token_id;
  rgw_get_token_id(token, token_id);

  return token_id;
}

namespace rgw {
namespace keystone {

enum class ApiVersion {
  VER_2,
  VER_3
};


class Config {
protected:
  Config() = default;
  virtual ~Config() = default;

public:
  virtual std::string get_endpoint_url() const noexcept = 0;
  virtual ApiVersion get_api_version() const noexcept = 0;

  virtual std::string get_admin_token() const noexcept = 0;
  virtual std::string_view get_admin_user() const noexcept = 0;
  virtual std::string get_admin_password() const noexcept = 0;
  virtual std::string_view get_admin_tenant() const noexcept = 0;
  virtual std::string_view get_admin_project() const noexcept = 0;
  virtual std::string_view get_admin_domain() const noexcept = 0;
};

class CephCtxConfig : public Config {
protected:
  CephCtxConfig() = default;
  virtual ~CephCtxConfig() = default;

  const static std::string empty;

public:
  static CephCtxConfig& get_instance() {
    static CephCtxConfig instance;
    return instance;
  }

  std::string get_endpoint_url() const noexcept override;
  ApiVersion get_api_version() const noexcept override;

  std::string get_admin_token() const noexcept override;

  std::string_view get_admin_user() const noexcept override {
    return g_ceph_context->_conf->rgw_keystone_admin_user;
  }

  std::string get_admin_password() const noexcept override;

  std::string_view get_admin_tenant() const noexcept override {
    return g_ceph_context->_conf->rgw_keystone_admin_tenant;
  }

  std::string_view get_admin_project() const noexcept override {
    return g_ceph_context->_conf->rgw_keystone_admin_project;
  }

  std::string_view get_admin_domain() const noexcept override {
    return g_ceph_context->_conf->rgw_keystone_admin_domain;
  }
};


class TokenEnvelope;
class TokenCache;

class Service {
public:
  class RGWKeystoneHTTPTransceiver : public RGWHTTPTransceiver {
  public:
    RGWKeystoneHTTPTransceiver(CephContext * const cct,
                               const std::string& method,
                               const std::string& url,
                               bufferlist * const token_body_bl)
      : RGWHTTPTransceiver(cct, method, url, token_body_bl,
                           cct->_conf->rgw_keystone_verify_ssl,
                           { "X-Subject-Token" }) {
    }

    const header_value_t& get_subject_token() const {
      try {
        return get_header_value("X-Subject-Token");
      } catch (std::out_of_range&) {
        static header_value_t empty_val;
        return empty_val;
      }
    }
  };

  typedef RGWKeystoneHTTPTransceiver RGWValidateKeystoneToken;
  typedef RGWKeystoneHTTPTransceiver RGWGetKeystoneAdminToken;

  static int get_admin_token(const DoutPrefixProvider *dpp,
                             CephContext* const cct,
                             TokenCache& token_cache,
                             const Config& config,
                             std::string& token);
  static int issue_admin_token_request(const DoutPrefixProvider *dpp,
                                       CephContext* const cct,
                                       const Config& config,
                                       TokenEnvelope& token);
  static int get_keystone_barbican_token(const DoutPrefixProvider *dpp,
                                         CephContext * const cct,
                                         std::string& token);
};


class TokenEnvelope {
public:
  class Domain {
  public:
    std::string id;
    std::string name;
    void decode_json(JSONObj *obj);
  };
  class Project {
  public:
    Domain domain;
    std::string id;
    std::string name;
    void decode_json(JSONObj *obj);
  };

  class Token {
  public:
    Token() : expires(0) { }
    std::string id;
    time_t expires;
    Project tenant_v2;
    void decode_json(JSONObj *obj);
  };

  class Role {
  public:
    std::string id;
    std::string name;
    void decode_json(JSONObj *obj);
  };

  class User {
  public:
    std::string id;
    std::string name;
    Domain domain;
    std::list<Role> roles_v2;
    void decode_json(JSONObj *obj);
  };

  Token token;
  Project project;
  User user;
  std::list<Role> roles;

  void decode_v3(JSONObj* obj);
  void decode_v2(JSONObj* obj);

public:
  /* We really need the default ctor because of the internals of TokenCache. */
  TokenEnvelope() = default;

  void set_expires(time_t expires) { token.expires = expires; }
  time_t get_expires() const { return token.expires; }
  const std::string& get_domain_id() const {return project.domain.id;};
  const std::string& get_domain_name() const {return project.domain.name;};
  const std::string& get_project_id() const {return project.id;};
  const std::string& get_project_name() const {return project.name;};
  const std::string& get_user_id() const {return user.id;};
  const std::string& get_user_name() const {return user.name;};
  bool has_role(const std::string& r) const;
  bool expired() const {
    const uint64_t now = ceph_clock_now().sec();
    return now >= static_cast<uint64_t>(get_expires());
  }
  int parse(const DoutPrefixProvider *dpp, CephContext* cct,
            const std::string& token_str,
            ceph::buffer::list& bl /* in */,
            ApiVersion version);
};


class TokenCache {
  struct token_entry {
    TokenEnvelope token;
    std::list<std::string>::iterator lru_iter;
  };

  std::atomic<bool> down_flag = { false };
  const boost::intrusive_ptr<CephContext> cct;

  std::string admin_token_id;
  std::string barbican_token_id;
  std::map<std::string, token_entry> tokens;
  std::map<std::string, token_entry> service_tokens;
  std::list<std::string> tokens_lru;
  std::list<std::string> service_tokens_lru;

  ceph::mutex lock = ceph::make_mutex("rgw::keystone::TokenCache");

  const size_t max;

  explicit TokenCache(const rgw::keystone::Config& config)
    : cct(g_ceph_context),
      max(cct->_conf->rgw_keystone_token_cache_size) {
  }

  ~TokenCache() {
    down_flag = true;
  }

public:
  TokenCache(const TokenCache&) = delete;
  void operator=(const TokenCache&) = delete;

  template<class ConfigT>
  static TokenCache& get_instance() {
    static_assert(std::is_base_of<rgw::keystone::Config, ConfigT>::value,
                  "ConfigT must be a subclass of rgw::keystone::Config");

    /* In C++11 this is thread safe. */
    static TokenCache instance(ConfigT::get_instance());
    return instance;
  }

  bool find(const std::string& token_id, TokenEnvelope& token);
  bool find_service(const std::string& token_id, TokenEnvelope& token);
  boost::optional<TokenEnvelope> find(const std::string& token_id) {
    TokenEnvelope token_envlp;
    if (find(token_id, token_envlp)) {
      return token_envlp;
    }
    return boost::none;
  }
  boost::optional<TokenEnvelope> find_service(const std::string& token_id) {
    TokenEnvelope token_envlp;
    if (find_service(token_id, token_envlp)) {
      return token_envlp;
    }
    return boost::none;
  }
  bool find_admin(TokenEnvelope& token);
  bool find_barbican(TokenEnvelope& token);
  void add(const std::string& token_id, const TokenEnvelope& token);
  void add_service(const std::string& token_id, const TokenEnvelope& token);
  void add_admin(const TokenEnvelope& token);
  void add_barbican(const TokenEnvelope& token);
  void invalidate(const DoutPrefixProvider *dpp, const std::string& token_id);
  bool going_down() const;
private:
  void add_locked(const std::string& token_id, const TokenEnvelope& token,
                  std::map<std::string, token_entry>& tokens, std::list<std::string>& tokens_lru);
  bool find_locked(const std::string& token_id, TokenEnvelope& token,
                   std::map<std::string, token_entry>& tokens, std::list<std::string>& tokens_lru);
};


class AdminTokenRequest {
public:
  virtual ~AdminTokenRequest() = default;
  virtual void dump(Formatter* f) const = 0;
};

class AdminTokenRequestVer2 : public AdminTokenRequest {
  const Config& conf;

public:
  explicit AdminTokenRequestVer2(const Config& conf)
    : conf(conf) {
  }
  void dump(Formatter *f) const override;
};

class AdminTokenRequestVer3 : public AdminTokenRequest {
  const Config& conf;

public:
  explicit AdminTokenRequestVer3(const Config& conf)
    : conf(conf) {
  }
  void dump(Formatter *f) const override;
};

class BarbicanTokenRequestVer2 : public AdminTokenRequest {
  CephContext *cct;

public:
  explicit BarbicanTokenRequestVer2(CephContext * const _cct)
    : cct(_cct) {
  }
  void dump(Formatter *f) const override;
};

class BarbicanTokenRequestVer3 : public AdminTokenRequest {
  CephContext *cct;

public:
  explicit BarbicanTokenRequestVer3(CephContext * const _cct)
    : cct(_cct) {
  }
  void dump(Formatter *f) const override;
};


}; /* namespace keystone */
}; /* namespace rgw */

#endif
