#pragma once

#include <optional>
#include <string>
#include <vector>

#include "environmental_grib/geo.h"

namespace environmental_grib {

struct Provider {
  std::string id;
  std::string label;
  std::optional<BoundingBox> coverage;
  std::optional<std::string> dataset_id;
  std::vector<std::string> variables;
  bool implemented{};
  std::string resolution;
  std::string description;
  std::optional<std::string> product_id;
  int default_step_hours{1};
  std::optional<double> minimum_depth;
  std::optional<double> maximum_depth;
  double source_grid_regularity_tolerance{1e-5};
  std::string provider_type{"model"};
  std::optional<int> nominal_duration_hours;
  std::optional<int> max_duration_hours;
  std::optional<std::string> source_url;

  [[nodiscard]] bool SupportsBbox(const BoundingBox& bbox) const;
};

class ProviderRegistry {
 public:
  ProviderRegistry();
  [[nodiscard]] const Provider& Get(const std::string& id) const;
  [[nodiscard]] const std::vector<Provider>& List() const { return providers_; }

 private:
  std::vector<Provider> providers_;
};

const Provider* SelectBestProviderForBbox(
    const BoundingBox& bbox, std::optional<int> duration_hours,
    const ProviderRegistry& registry);
const Provider& SelectCopernicusProvider(const std::string& provider_id,
                                         const BoundingBox& bbox,
                                         const ProviderRegistry& registry);

}  // namespace environmental_grib

