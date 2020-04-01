/*
 * Copyright (C) 2020 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <memory>

#include <sdf/sdf.hh>

#include <ignition/common/URI.hh>

#include "ignition/gazebo/Util.hh"
#include "ignition/gazebo/components/Light.hh"
#include "ignition/gazebo/components/Model.hh"
#include "ignition/gazebo/components/Name.hh"
#include "ignition/gazebo/components/Pose.hh"
#include "ignition/gazebo/components/World.hh"

#include "SdfGenerator.hh"

namespace ignition
{
namespace gazebo
{
// Inline bracket to help doxygen filtering.
inline namespace IGNITION_GAZEBO_VERSION_NAMESPACE
{
namespace sdf_generator
{
  class Benchmark
  {
    public: Benchmark(std::string _str) : str(std::move(_str))
    {
      tInit = common::Time::SystemTime();
    }

    public: ~Benchmark()
    {
      tFin = common::Time::SystemTime();
      std::cout
          << str << " "
          << (tFin - tInit).FormattedString(common::Time::FormatOption::SECONDS)
          << std::endl;
    }

    private: common::Time tInit, tFin;
    private: std::string str;
  };

  /////////////////////////////////////////////////
  /// \brief Copy sdf::Element from component
  /// \param[in] _comp Component containing an sdf::Element. The component can
  /// be a DOM object or an sdf::Element
  /// \param[out] _elem Output sdf::Element
  /// \returns False if the component is nullptr
  template <typename ComponentT>
  static bool copySdf(ComponentT *_comp, const sdf::ElementPtr &_elem)
  {
    if (nullptr != _comp)
    {
      if constexpr (std::is_same<typename ComponentT::Type,
                                 sdf::ElementPtr>::value)
      {
        _elem->Copy(_comp->Data());
      }
      else
      {
        _elem->Copy(_comp->Data().Element());
      }
      return true;
    }
    return false;
  }

  /////////////////////////////////////////////////
  /// \brief Remove version number from Fuel URI
  /// \param[in, out] _uri The URI from which the version number is removed.
  /// This is assumed to be a Fuel URI where, if there is a version number, it's
  /// the last part of the URI.
  static void removeVersionFromUri(common::URI &_uri)
  {
    auto uriSplit = common::split(common::trimmed(_uri.Path().Str()), "/");
    if (uriSplit.size() > 0)
    {
      try {
        std::stol(uriSplit.back());
        uriSplit.pop_back();
        common::URIPath newPath;
        for (const auto &segment: uriSplit)
        {
          newPath /= segment;
        }
        _uri.Path() = newPath;
      }
      catch(const std::invalid_argument &)
      {
      }
    }
  }

  /////////////////////////////////////////////////
  /// \brief Determine if a model is inlined or included based on the file paths
  /// of the model and the world.
  /// \param[in] _modelDir Directory containing model
  /// \param[in] _worldDir Directory containing world
  /// \returns True if the source of the model is a separate file that was
  /// included into the world using the `<include>` tag.
  static bool isModelFromInclude(const std::string &_modelDir,
                                 const std::string _worldDir)
  {
    // There are several cases to consider here
    // modelDir == "" , worldDir == ""
    //  - This is the case where the world and the model were loaded from
    //  a string, so the modelFromInclude = false
    // modelDir == "/some/path", worldDir == ""
    //  - The world was loaded from a string, but the model as included,
    //  so modelFromInclude = true
    // modelDir == "", worldDir == "/some/path"
    //  - The world was loaded from a file, but the model was loaded from
    //  a string (via UserCommands, for example), so modelFromInclude =
    //  false
    // modelDir == "/some/path", worldDir == "/some/path"
    //  - Both world and model were loaded from file. There two subcases
    //    - modelDir == worldDir
    //      - Model was inlined in the world file, so modelFromInclude =
    //      false
    //    - modelDir != worldDir
    //      - Model was loaded from a different file, so modelFromInclude
    //      = true
    //
    if (!_modelDir.empty() && _worldDir.empty())
    {
      return true;
    }
    else if (!_modelDir.empty() && !_worldDir.empty() &&
        (_modelDir != _worldDir))
    {
      return true;
    }

    return false;
  }

  /////////////////////////////////////////////////
  /// \brief Merge a configuration with another while making sure any parameters
  /// set in `_override` are copied as is.
  ///
  /// The idea here is that the initial configuration is a global configuration
  /// that can be overriden on a per model basis. The following snippet
  /// demonstrates the intent, even though it won't compile
  /// \code
  ///   ModelGeneratorConfig initialConfig;
  ///   initialConfig.expand_include_tags = true
  ///   initialConfig.save_fuel_model_version = true
  ///
  ///   ModelGeneratorConfig overrideConfig;
  ///   overrideConfig.expand_include_tags = false;
  ///
  ///   ModelGeneratorConfig combinedConfig = initialConfig;
  ///   mergeWithOverride(combinedConfig, overrideConfig);
  /// \endcode
  ///
  /// The contents of each of the configs is now:
  /// \code
  ///   initialConfig = {
  ///     expand_include_tags = true
  ///     save_fuel_model_version = true
  ///   }
  ///   overrideConfig = {
  ///     expand_include_tags = false
  ///     save_fuel_model_version = false (unset, but defaults to false)
  ///   }
  ///
  ///   combinedConfig = {
  ///     expand_include_tags = false
  ///     save_fuel_model_version = true
  ///   }
  /// \endcode
  ///
  /// This function is needed because neither Message::CopyFrom nor
  /// Message::MergeFrom do what we want. CopyFrom overwrites everything even if
  /// none of the parameters in overrideConfig are set. MergeFrom gets close,
  /// but doesn't overrwrite if the parameter in overrideConfig is set to false.
  ///
  /// \param[in, out] _initialConfig Initial configuration
  /// \param[in] _override Override configuration
  static void mergeWithOverride(
      msgs::SdfGeneratorConfig::ModelGeneratorConfig &_initialConfig,
      const msgs::SdfGeneratorConfig::ModelGeneratorConfig &_overrideConfig)
  {
    auto initialDesc = _initialConfig.GetDescriptor();
    auto initialRefl = _initialConfig.GetReflection();
    auto overrideDesc = _overrideConfig.GetDescriptor();
    auto overrideRefl = _overrideConfig.GetReflection();

    for (int i = 0; i < overrideDesc->field_count(); ++i)
    {
      // If a field is set in _overrideConfig, copy it over to initialConfig
      // overwriting anything there
      if (overrideRefl->HasField(_overrideConfig, overrideDesc->field(i)))
      {
        initialRefl->MutableMessage(&_initialConfig, initialDesc->field(i))
            ->CopyFrom(overrideRefl->GetMessage(
                _overrideConfig, overrideDesc->field(i)));
      }
    }
  }

  /////////////////////////////////////////////////
  std::optional<std::string> generateWorld(
      const EntityComponentManager &_ecm, const Entity &_entity,
      const IncludeUriMap &_includeUriMap,
      const msgs::SdfGeneratorConfig &_config)
  {
    Benchmark bm(__func__);
    sdf::ElementPtr elem = std::make_shared<sdf::Element>();
    sdf::initFile("root.sdf", elem);
    auto worldElem = elem->AddElement("world");
    if (!updateWorldElement(worldElem, _ecm, _entity, _includeUriMap, _config))
      return std::nullopt;

    return elem->ToString("");
  }

  /////////////////////////////////////////////////
  bool updateWorldElement(sdf::ElementPtr _elem,
                          const EntityComponentManager &_ecm,
                          const Entity &_entity,
                          const IncludeUriMap &_includeUriMap,
                          const msgs::SdfGeneratorConfig &_config)
  {
    const components::WorldSdf *worldSdf =
        _ecm.Component<components::WorldSdf>(_entity);

    if (nullptr == worldSdf)
      return false;

    if (!copySdf(_ecm.Component<components::WorldSdf>(_entity), _elem))
      return false;

    // Remove all models. TODO (addisu) Remove actors as well.
    std::vector<sdf::ElementPtr> toRemove;
    if (_elem->HasElement("model"))
    {
      for (auto modelElem = _elem->GetElement("model"); modelElem;
           modelElem = modelElem->GetNextElement("model"))
      {
        toRemove.push_back(modelElem);
      }
    }
    for (auto e : toRemove)
    {
      _elem->RemoveChild(e);
    }

    auto worldDir = common::parentPath(worldSdf->Data().Element()->FilePath());

    _ecm.Each<components::Model, components::ModelSdf>(
        [&](const Entity &_modelEntity, const components::Model *,
            const components::ModelSdf *_modelSdf)
        {
          auto modelDir =
              common::parentPath(_modelSdf->Data().Element()->FilePath());
          const std::string modelName =
              scopedName(_modelEntity, _ecm, "::", false);

          bool modelFromInclude = isModelFromInclude(modelDir, worldDir);

          auto uriMapIt = _includeUriMap.find(modelDir);

          auto modelConfig = _config.global_model_gen_config();
          auto modelConfigIt =
              _config.override_model_gen_configs().find(modelName);
          if (modelConfigIt != _config.override_model_gen_configs().end())
          {
            mergeWithOverride(modelConfig, modelConfigIt->second);
          }

          if (modelConfig.expand_include_tags().data() || !modelFromInclude)
          {
            auto modelElem = _elem->AddElement("model");
            updateModelElement(modelElem, _ecm, _modelEntity);
          }
          else if (uriMapIt != _includeUriMap.end())
          {
            // The fuel URI might have a version number. If it does, we remove
            // it unless saveFuelModelVersion is set to false.
            // Check if this is a fuel URI. We assume that it is a fuel URI if
            // the scheme is http or https.
            common::URI uri(uriMapIt->second);
            if (uri.Scheme() == "http" || uri.Scheme() == "https")
            {
              removeVersionFromUri(uri);
            }

            if (modelConfig.save_fuel_model_version().data())
            {
              // Find out the model version from the file path. Note that we
              // do this from the file path instead of the Fuel URI because the
              // URI may not contain version information.
              // We are assuming here that, for Fuel models, the directory
              // containing the sdf file has the same name as the model version.
              // TODO (addisu) Verify this assumption
              uri.Path() /= common::basename(modelDir);
            }

            auto includeElem = _elem->AddElement("include");
            updateIncludeElement(includeElem, _ecm, _modelEntity, uri.Str());
          }
          else
          {
            // The model is not in the includeUriMap, but expandIncludeTags =
            // false, so we will assume that its uri is the file path of the
            // model on the local machine
            auto includeElem = _elem->AddElement("include");
            const std::string uri = "file://" + modelDir;
            updateIncludeElement(includeElem, _ecm, _modelEntity, uri);
          }
          return true;
        });

    return true;
  }

  /////////////////////////////////////////////////
  bool updateModelElement(sdf::ElementPtr _elem,
                          const EntityComponentManager &_ecm,
                          const Entity &_entity)
  {
    if(!copySdf(_ecm.Component<components::ModelSdf>(_entity), _elem))
      return false;

    // Update sdf based current components. Here are the list of components to
    // be update:
    // - Pose
    // This list is to be updated as other components become updateable during
    // simulation
    auto *poseComp = _ecm.Component<components::Pose>(_entity);

    auto poseElem = _elem->GetElement("pose");

    // Remove all attributes of poseElem
    // TODO (addisu): Uncomment this for sdformat 1.7
    // sdf::ParamPtr relativeTo = poseElem->GetAttribute("relative_to");
    // if (nullptr != relativeTo)
    // {
    //   relativeTo->Reset();
    // }
    poseElem->Set(poseComp->Data());
    return true;
  }

  /////////////////////////////////////////////////
  bool updateIncludeElement(sdf::ElementPtr _elem,
                            const EntityComponentManager &_ecm,
                            const Entity &_entity, std::string _uri)
  {
    _elem->GetElement("uri")->Set(_uri);

    auto *nameComp = _ecm.Component<components::Name>(_entity);
    _elem->GetElement("name")->Set(nameComp->Data());

    auto *poseComp = _ecm.Component<components::Pose>(_entity);

    auto poseElem = _elem->GetElement("pose");

    // Remove all attributes of poseElem
    // TODO (addisu): Uncomment this for sdformat 1.7
    // sdf::ParamPtr relativeTo = poseElem->GetAttribute("relative_to");
    // if (nullptr != relativeTo)
    // {
    //   relativeTo->Reset();
    // }
    poseElem->Set(poseComp->Data());
    return true;
  }
}
}  // namespace IGNITION_GAZEBO_VERSION_NAMESPACE
}  // namespace gazebo
}  // namespace ignition
