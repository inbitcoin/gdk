#include "session_impl.hpp"
#include "exception.hpp"
#include "ga_rust.hpp"
#include "ga_session.hpp"
#include "logging.hpp"

namespace ga {
namespace sdk {

    namespace {
        template <typename T>
        static void set_override(nlohmann::json& ret, const std::string& key, const nlohmann::json& src, T default_)
        {
            // Use the users provided value, else the registered value, else `default_`
            ret[key] = src.value(key, ret.value(key, default_));
        }

        static network_parameters get_network_overrides(const nlohmann::json& user_params, nlohmann::json& defaults)
        {
            // Set override-able settings from the users parameters
            set_override(defaults, "electrum_tls", user_params, false);
            set_override(defaults, "electrum_url", user_params, std::string());
            set_override(defaults, "log_level", user_params, "none");
            set_override(defaults, "spv_multi", user_params, false);
            set_override(defaults, "spv_servers", user_params, nlohmann::json::array());
            set_override(defaults, "spv_enabled", user_params, false);
            set_override(defaults, "use_tor", user_params, false);
            set_override(defaults, "user_agent", user_params, std::string());
            set_override(defaults, "cert_expiry_threshold", user_params, 1);
            return network_parameters{ defaults };
        }

        static void configure_logging(const network_parameters& net_params)
        {
            const auto level = net_params.log_level();
            // Default to fatal logging, i.e. 'none' since we don't log any
            auto severity = log_level::severity_level::fatal;
            if (level == "debug") {
                severity = log_level::severity_level::debug;
            } else if (level == "info") {
                severity = log_level::severity_level::info;
            } else if (level == "warn") {
                severity = log_level::severity_level::warning;
            } else if (level == "error") {
                severity = log_level::severity_level::error;
            }
            boost::log::core::get()->set_filter(log_level::severity >= severity);
        }
    } // namespace

    boost::shared_ptr<session_impl> session_impl::create(const nlohmann::json& net_params)
    {
        auto defaults = network_parameters::get(net_params.value("name", std::string()));
        const auto type = net_params.value("server_type", defaults.value("server_type", std::string()));

        if (type == "green") {
            return boost::make_shared<ga_session>(net_params, defaults);
        }
#ifdef BUILD_GDK_RUST
        if (type == "electrum") {
            return boost::make_shared<ga_rust>(net_params, defaults);
        }
#endif
        throw user_error("Unknown server_type");
    }

    session_impl::session_impl(const nlohmann::json& net_params, nlohmann::json& defaults)
        : m_net_params(get_network_overrides(net_params, defaults))
        , m_debug_logging(m_net_params.log_level() == "debug")
        , m_notification_handler(nullptr)
        , m_notification_context(nullptr)
    {
        configure_logging(m_net_params);
    }

    session_impl::~session_impl() {}

    void session_impl::set_notification_handler(GA_notification_handler handler, void* context)
    {
        m_notification_handler = handler;
        m_notification_context = context;
    }

    void session_impl::emit_notification(nlohmann::json details, bool /*async*/)
    {
        // By default, ignore the async flag
        if (m_notification_handler) {
            // We use 'new' here as it is the handlers responsibility to 'delete'
            const auto details_p = reinterpret_cast<GA_json*>(new nlohmann::json(details));
            m_notification_handler(m_notification_context, details_p);
        }
    }

    void session_impl::register_user(const std::string& /*master_pub_key_hex*/,
        const std::string& /*master_chain_code_hex*/, const std::string& /*gait_path_hex*/, bool /*supports_csv*/)
    {
        // Default impl is a no-op; registration is only meaningful in multisig
    }

    nlohmann::json session_impl::login(std::shared_ptr<signer> /*signer*/)
    {
        GDK_RUNTIME_ASSERT(false); // Only used by rust until it supports HWW
        return nlohmann::json();
    }

    bool session_impl::set_blinding_nonce(
        const std::string& /*pubkey_hex*/, const std::string& /*script_hex*/, const std::string& /*nonce_hex*/)
    {
        return false; // No nonce caching by default, so return 'not updated'
    }

    bool session_impl::get_uncached_blinding_nonces(
        const nlohmann::json& /*details*/, nlohmann::json& /*twofactor_data*/)
    {
        // Implementation detail of ga_session
        return false;
    }

    void session_impl::save_cache()
    {
        // Refers to the ga_session cache at the moment, so a no-op for rust sessions
    }

    session_impl::utxo_cache_value_t session_impl::get_cached_utxos(uint32_t subaccount, uint32_t num_confs) const
    {
        locker_t locker(m_utxo_cache_mutex);
        // FIXME: If we have no unconfirmed txs, 0 and 1 conf results are
        // identical, so we could share 0 & 1 conf storage
        auto p = m_utxo_cache.find({ subaccount, num_confs });
        return p == m_utxo_cache.end() ? utxo_cache_value_t() : p->second;
    }

    session_impl::utxo_cache_value_t session_impl::set_cached_utxos(
        uint32_t subaccount, uint32_t num_confs, nlohmann::json& utxos)
    {
        // Convert null UTXOs into an empty element
        auto& outputs = utxos.at("unspent_outputs");
        if (outputs.is_null()) {
            outputs = nlohmann::json::object();
        }
        // Encache
        locker_t locker(m_utxo_cache_mutex);
        auto entry = std::make_shared<const nlohmann::json>(std::move(utxos));
        m_utxo_cache[std::make_pair(subaccount, num_confs)] = entry;
        return entry;
    }

    void session_impl::remove_cached_utxos(const std::vector<uint32_t>& subaccounts)
    {
        std::vector<utxo_cache_value_t> tmp_values; // Delete outside of lock
        utxo_cache_t tmp_cache;
        {
            locker_t locker(m_utxo_cache_mutex);
            if (subaccounts.empty()) {
                // Empty subaccount list means clear the entire cache
                std::swap(m_utxo_cache, tmp_cache);
            } else {
                // Remove all entries for affected subaccounts
                for (auto p = m_utxo_cache.begin(); p != m_utxo_cache.end(); /* no-op */) {
                    if (std::find(subaccounts.begin(), subaccounts.end(), p->first.first) != subaccounts.end()) {
                        tmp_values.push_back(p->second);
                        m_utxo_cache.erase(p++);
                    } else {
                        ++p;
                    }
                }
            }
        }
    }

    void session_impl::process_unspent_outputs(nlohmann::json& /*utxos*/)
    {
        // Only needed for multisig until singlesig supports HWW
    }

    std::shared_ptr<signer> session_impl::get_nonnull_signer()
    {
        auto signer = get_signer();
        GDK_RUNTIME_ASSERT(signer != nullptr);
        return signer;
    }

    std::shared_ptr<signer> session_impl::get_signer()
    {
        locker_t locker(m_mutex);
        return m_signer;
    }

    void session_impl::encache_signer_xpubs(std::shared_ptr<signer> /*signer*/)
    {
        // Overriden for multisig
    }

    std::pair<std::string, bool> session_impl::get_cached_master_blinding_key()
    {
        // Overriden for multisig
        return std::make_pair(std::string(), false);
    }

    void session_impl::set_cached_master_blinding_key(const std::string& /*master_blinding_key_hex*/)
    {
        // Overriden for multisig
    }

} // namespace sdk
} // namespace ga
